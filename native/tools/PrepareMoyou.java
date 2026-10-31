// Ghidra pre-analysis: verified relocated PRX image, guest TOC, function seeds.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.MemoryBlock;
import java.math.BigInteger;
import java.nio.file.Files;
import java.nio.file.Path;

public class PrepareMoyou extends GhidraScript {
    @Override protected void run() throws Exception {
        String[] args = getScriptArgs();
        var context = currentProgram.getProgramContext();
        context.setValue(currentProgram.getRegister("r2"), currentProgram.getMinAddress(),
            currentProgram.getMaxAddress(), new BigInteger(args[1].replace("0x", ""), 16));
        var memory = currentProgram.getMemory();
        var boundary = toAddr(Long.decode(args[2]));
        MemoryBlock code = memory.getBlock(boundary);
        memory.split(code, boundary);
        memory.getBlock(currentProgram.getMinAddress()).setExecute(true);
        memory.getBlock(boundary).setExecute(false);
        for (String line : Files.readAllLines(Path.of(args[0]))) {
            var address = toAddr(Long.decode(line.strip()));
            if (address.compareTo(boundary) >= 0) continue;
            disassemble(address);
            createFunction(address, null);
        }
    }
}
