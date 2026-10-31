// Ghidra post-analysis: keep all firmware-derived output in the local build.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.ArrayDeque;

public class ExportMoyou extends GhidraScript {
    @Override protected void run() throws Exception {
        String[] args = getScriptArgs();
        Path output = Path.of(args[0]);
        Files.createDirectories(output);
        var decompiler = new DecompInterface();
        if (!decompiler.openProgram(currentProgram)) throw new IllegalStateException("Cannot open original program");
        try {
            var bounds = new ArrayList<String>();
            var functions = currentProgram.getFunctionManager().getFunctions(true);
            while (functions.hasNext()) {
                var function = functions.next();
                bounds.add(function.getEntryPoint() + " " + function.getBody().getMaxAddress().add(1));
            }
            Files.write(output.resolve("function-bounds.txt"), bounds);
            for (int i = 1; i < args.length; ++i) {
                if (args[i].startsWith("@")) {
                    var pending = new ArrayDeque<ghidra.program.model.address.Address>();
                    var visited = new HashSet<ghidra.program.model.address.Address>();
                    pending.add(toAddr(Long.decode(args[i].substring(1))));
                    while (!pending.isEmpty() && visited.size() < 1000) {
                        var target = pending.remove();
                        if (!visited.add(target)) continue;
                        var references = currentProgram.getReferenceManager().getReferencesTo(target);
                        while (references.hasNext()) {
                            var from = references.next().getFromAddress();
                            var function = getFunctionContaining(from);
                            if (function == null) { pending.add(from); continue; }
                            println("REFERENCE " + target + " <- " + from + " in " + function.getEntryPoint());
                            var result = decompiler.decompileFunction(function, 30, monitor);
                            if (result.decompileCompleted())
                                Files.writeString(output.resolve(function.getEntryPoint() + ".c"), result.getDecompiledFunction().getC());
                        }
                    }
                    continue;
                }
                if (args[i].contains(":")) {
                    String[] range = args[i].split(":");
                    var begin = toAddr(Long.decode(range[0]));
                    var end = toAddr(Long.decode(range[1]));
                    var selection = currentProgram.getFunctionManager().getFunctions(begin, true);
                    while (selection.hasNext()) {
                        var selected = selection.next();
                        if (selected.getEntryPoint().compareTo(end) >= 0) break;
                        var result = decompiler.decompileFunction(selected, 30, monitor);
                        if (result.decompileCompleted())
                            Files.writeString(output.resolve(selected.getEntryPoint() + ".c"), result.getDecompiledFunction().getC());
                    }
                    continue;
                }
                var address = toAddr(Long.decode(args[i]));
                var function = getFunctionAt(address);
                if (function == null) function = getFunctionContaining(address);
                if (function == null) throw new IllegalStateException("Missing function at " + args[i]);
                var result = decompiler.decompileFunction(function, 60, monitor);
                if (!result.decompileCompleted()) throw new IllegalStateException(result.getErrorMessage());
                Files.writeString(output.resolve(function.getEntryPoint() + ".c"), result.getDecompiledFunction().getC());
            }
        } finally { decompiler.dispose(); }
    }
}
