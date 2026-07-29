// DumpPitchWriters - decompile the code that a hardware write breakpoint caught writing
// the player controller's view pitch.
//
// A HW breakpoint on controller+0x60 during mouse look reported these EIPs. Everything
// below is inside the game image (base 0x400000, no ASLR), so the runtime addresses map
// one-to-one onto this database.
//
// Run:  .\tools\ghidra.ps1 -Script DumpPitchWriters.java
//@category Singularity

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.io.PrintWriter;

public class DumpPitchWriters extends GhidraScript {

    // caught by the hardware write breakpoint, most interesting first
    // 0x0104e390 is the prize: the instruction stream at 0x01034105 CALLs it and stores the
    // FRotator it returns straight into the controller's rotation. That makes it the view
    // rotation SOURCE, and therefore the correct detour target.
    private static final String[] EIPS = {
        "0104e390",   // <-- view rotation source
        "0104e420",   // the neighbouring call seen in the same region
    };

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter("R:\\SingularityVR-Dev\\ghidra_projects\\pitch_writers.txt");

        for (String e : EIPS) {
            Address a = addr(e);
            emit(out, "");
            emit(out, "==================================================");
            emit(out, " writer EIP " + a);
            emit(out, "==================================================");

            Function f = getFunctionContaining(a);
            if (f == null) {
                emit(out, "  no containing function");
                Instruction i = currentProgram.getListing().getInstructionContaining(a);
                emit(out, i == null ? "  (no instruction)" : ("  instruction: " + i));
                continue;
            }
            emit(out, "  function " + f.getName() + "  entry=" + f.getEntryPoint() +
                      "  size=" + f.getBody().getNumAddresses() +
                      "  offset +0x" + Long.toHexString(a.subtract(f.getEntryPoint())));

            // instructions immediately around the write
            emit(out, "  --- disassembly around the write ---");
            Instruction ins = currentProgram.getListing().getInstructionContaining(a);
            Instruction cur = ins;
            for (int i = 0; i < 12 && cur != null; i++) {
                Instruction p = cur.getPrevious();
                if (p == null || !f.getBody().contains(p.getAddress())) break;
                cur = p;
            }
            for (int i = 0; i < 26 && cur != null; i++) {
                String mark = cur.getAddress().equals(ins.getAddress()) ? "  >>>> " : "        ";
                emit(out, mark + cur.getAddress() + "  " + cur);
                cur = cur.getNext();
            }

            // who calls this function - context for where in the frame it runs
            emit(out, "  --- callers ---");
            ReferenceIterator it = currentProgram.getReferenceManager()
                                     .getReferencesTo(f.getEntryPoint());
            int n = 0;
            while (it.hasNext() && n < 12) {
                Reference r = it.next();
                Function cf = getFunctionContaining(r.getFromAddress());
                emit(out, "     " + r.getFromAddress() +
                          (cf != null ? ("  in " + cf.getName() + " (size " + cf.getBody().getNumAddresses() + ")")
                                      : "  (no function)"));
                n++;
            }
            if (n == 0) emit(out, "     (none - reached indirectly)");

            emit(out, "  --- decompiled ---");
            decompile(out, f);
        }

        out.close();
        println("wrote R:\\SingularityVR-Dev\\ghidra_projects\\pitch_writers.txt");
    }

    private void decompile(PrintWriter out, Function f) {
        if (f.getBody().getNumAddresses() > 24000) { emit(out, "  (too large to decompile)"); return; }
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        DecompileResults r = di.decompileFunction(f, 120, monitor);
        emit(out, (r != null && r.decompileCompleted()) ? r.getDecompiledFunction().getC() : "  (decompile failed)");
        di.dispose();
    }

    private void emit(PrintWriter out, String s) { out.println(s); println(s); }

    private Address addr(String hex) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace()
                .getAddress(Long.parseLong(hex, 16));
    }
}
