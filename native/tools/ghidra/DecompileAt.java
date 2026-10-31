// Decompile functions at given addresses (creating them if missing) into args[0].
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.cmd.function.CreateFunctionCmd;
import java.nio.file.Files;
import java.nio.file.Path;

public class DecompileAt extends GhidraScript {
    @Override protected void run() throws Exception {
        String[] args = getScriptArgs();
        Path output = Path.of(args[0]);
        Files.createDirectories(output);
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        for (int i = 1; i < args.length; ++i) {
            var address = toAddr(args[i]);
            var function = getFunctionAt(address);
            if (function == null) {
                disassemble(address);
                new CreateFunctionCmd(address).applyTo(currentProgram);
                function = getFunctionAt(address);
            }
            if (function == null) { println("No function at " + args[i]); continue; }
            var result = decompiler.decompileFunction(function, 120, monitor);
            String text = result.decompileCompleted() ? result.getDecompiledFunction().getC() : "// failed: " + result.getErrorMessage();
            Files.writeString(output.resolve(args[i].replace("0x", "") + ".c"), text);
        }
    }
}
