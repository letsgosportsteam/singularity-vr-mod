// DumpCrashSite - disassemble and decompile the function around the repeated crash.
//
// Six identical 0xC0000005 faults at 0x00BE6550 during the pool-probe session. The address
// lies inside Singularity.exe's own image, not in any injected module. Find out what code
// lives there and who calls it.
//
// Run:  .\tools\ghidra.ps1 -Script DumpCrashSite.java
//@category Singularity

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.io.PrintWriter;

public class DumpCrashSite extends GhidraScript {

    private static final String CRASH_ADDR = "00be6550";

    @Override
    public void run() throws Exception {
        String outPath = "R:\\SingularityVR-Dev\\ghidra_projects\\crash_site.txt";
        PrintWriter out = new PrintWriter(outPath);

        Address crash = addr(CRASH_ADDR);
        emit(out, "=== crash site analysis ===");
        emit(out, "faulting address: " + crash);
        emit(out, "image base:       " + currentProgram.getImageBase());

        Listing listing = currentProgram.getListing();
        Function f = getFunctionContaining(crash);

        if (f == null) {
            emit(out, "");
            emit(out, "NO FUNCTION contains this address.");
            Instruction ins = listing.getInstructionContaining(crash);
            emit(out, ins == null ? "and no instruction either - it is data or unanalysed."
                                  : "instruction here: " + ins);
            out.close();
            return;
        }

        emit(out, "");
        emit(out, "function:  " + f.getName());
        emit(out, "entry:     " + f.getEntryPoint());
        emit(out, "size:      " + f.getBody().getNumAddresses() + " bytes");
        emit(out, "offset into function: 0x" +
                  Long.toHexString(crash.subtract(f.getEntryPoint())));

        // ---- who calls this function? ----
        emit(out, "");
        emit(out, "--- callers ---");
        ReferenceIterator it = currentProgram.getReferenceManager()
                                 .getReferencesTo(f.getEntryPoint());
        int n = 0;
        while (it.hasNext() && n < 40) {
            Reference r = it.next();
            Function cf = getFunctionContaining(r.getFromAddress());
            emit(out, "  " + r.getFromAddress() + "  " + r.getReferenceType() +
                      (cf != null ? ("  in " + cf.getName()) : ""));
            n++;
        }
        if (n == 0) emit(out, "  (none found - likely called indirectly, e.g. through a vtable)");

        // ---- disassembly around the fault ----
        emit(out, "");
        emit(out, "--- disassembly around the faulting instruction ---");
        Instruction ins = listing.getInstructionContaining(crash);
        Instruction cur = ins;
        for (int i = 0; i < 14 && cur != null; i++) {
            Instruction prev = cur.getPrevious();
            if (prev == null || !f.getBody().contains(prev.getAddress())) break;
            cur = prev;
        }
        for (int i = 0; i < 30 && cur != null; i++) {
            String mark = cur.getAddress().equals(crash) ? "  >>>> FAULT " : "        ";
            emit(out, mark + cur.getAddress() + "  " + cur.toString());
            cur = cur.getNext();
        }

        // ---- decompile ----
        if (f.getBody().getNumAddresses() < 30000) {
            emit(out, "");
            emit(out, "--- decompiled ---");
            DecompInterface di = new DecompInterface();
            di.openProgram(currentProgram);
            DecompileResults res = di.decompileFunction(f, 120, monitor);
            if (res != null && res.decompileCompleted()) {
                emit(out, res.getDecompiledFunction().getC());
            } else {
                emit(out, "(decompilation failed)");
            }
            di.dispose();
        }

        out.close();
        println("wrote " + outPath);
    }

    private void emit(PrintWriter out, String s) { out.println(s); println(s); }

    private Address addr(String hex) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace()
                .getAddress(Long.parseLong(hex, 16));
    }
}
