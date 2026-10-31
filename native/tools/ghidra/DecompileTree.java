// Decompile functions reachable from the given roots (depth-bounded) into args[0]; list them in index.txt.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.listing.Function;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.*;

public class DecompileTree extends GhidraScript {
    @Override protected void run() throws Exception {
        String[] args = getScriptArgs();
        Path output = Path.of(args[0]);
        int depthLimit = Integer.parseInt(args[1]);
        Files.createDirectories(output);
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        ArrayDeque<Object[]> queue = new ArrayDeque<>();
        for (int i = 2; i < args.length; ++i) queue.add(new Object[]{toAddr(args[i]), 0});
        Set<String> done = new TreeSet<>();
        StringBuilder index = new StringBuilder(), bounds = new StringBuilder();
        while (!queue.isEmpty()) {
            Object[] item = queue.poll();
            var address = (ghidra.program.model.address.Address) item[0];
            int depth = (Integer) item[1];
            if (!done.add(address.toString())) continue;
            Function function = getFunctionAt(address);
            if (function == null) { disassemble(address); new CreateFunctionCmd(address).applyTo(currentProgram); function = getFunctionAt(address); }
            if (function == null) continue;
            var result = decompiler.decompileFunction(function, 120, monitor);
            String text = result.decompileCompleted() ? result.getDecompiledFunction().getC() : "// failed: " + result.getErrorMessage();
            Files.writeString(output.resolve(address.toString().replace("0x","") + ".c"), text);
            long size = function.getBody().getNumAddresses();
            bounds.append(String.format("0x%08X 0x%08X%n", function.getBody().getMinAddress().getOffset(), function.getBody().getMaxAddress().getOffset() + 1));
            index.append(address).append(' ').append(depth).append(' ').append(size).append(' ').append(function.getName()).append('\n');
            if (depth >= depthLimit) continue;
            for (Function callee : function.getCalledFunctions(monitor)) queue.add(new Object[]{callee.getEntryPoint(), depth + 1});
        }
        Files.writeString(output.resolve("index.txt"), index.toString());
        Files.writeString(output.resolve("function-bounds.txt"), bounds.toString());
    }
}
