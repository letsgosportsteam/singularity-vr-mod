// DumpCameraRegistration - read the UE3 native-registration site around the camera names.
//
// FindCameraHooks showed FUN_0090f770 references CalcCamera, GetPlayerViewPoint and
// ProcessViewRotation. In UE3 the native binding is a name paired with a function
// pointer, so the instructions immediately around each name reference should hand us the
// actual implementation address. Dump that context and flag any operand that resolves
// into a real function.
//
// Run:  .\tools\ghidra.ps1 -Script DumpCameraRegistration.java
//@category Singularity

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.lang.OperandType;

import java.io.PrintWriter;

public class DumpCameraRegistration extends GhidraScript {

    // xref sites reported by FindCameraHooks, plus the functions worth reading whole.
    private static final String[] XREF_SITES = {
        "0090fca5",  // CalcCamera
        "00911207",  // GetPlayerViewPoint
        "0091345a",  // ProcessViewRotation
    };

    private static final String[] FUNCS_OF_INTEREST = {
        "0090f770",  // the registration function referencing all three
        "00f24770",  // RvPlayerCamera
        "01040f10",  // RvPlayerController
    };

    private static final int CONTEXT_BEFORE = 12;
    private static final int CONTEXT_AFTER  = 12;

    @Override
    public void run() throws Exception {
        String outPath = "R:\\SingularityVR-Dev\\ghidra_projects\\camera_registration.txt";
        PrintWriter out = new PrintWriter(outPath);

        emit(out, "=== camera registration dump: " + currentProgram.getName() + " ===");
        emit(out, "image base " + currentProgram.getImageBase());

        Listing listing = currentProgram.getListing();

        // --- sizes first: a giant function means a table, a small one means a thunk ---
        emit(out, "");
        emit(out, "--- candidate function sizes ---");
        for (String fa : FUNCS_OF_INTEREST) {
            Address a = addr(fa);
            Function f = getFunctionContaining(a);
            if (f == null) { emit(out, "  " + fa + ": no function"); continue; }
            long size = f.getBody().getNumAddresses();
            emit(out, String.format("  %s  %s  size=%d bytes  params=%d",
                    fa, f.getName(), size, f.getParameterCount()));
        }

        // --- instruction context around each name reference ---
        for (String site : XREF_SITES) {
            if (monitor.isCancelled()) break;
            Address at = addr(site);
            emit(out, "");
            emit(out, "==================================================");
            emit(out, " context around " + site);
            emit(out, "==================================================");

            Instruction ins = listing.getInstructionContaining(at);
            if (ins == null) { emit(out, "  (no instruction here - it is data)"); continue; }

            // rewind
            Instruction cur = ins;
            for (int i = 0; i < CONTEXT_BEFORE && cur != null; i++) {
                Instruction prev = cur.getPrevious();
                if (prev == null) break;
                cur = prev;
            }
            for (int i = 0; i < CONTEXT_BEFORE + CONTEXT_AFTER + 1 && cur != null; i++) {
                String marker = cur.getAddress().equals(ins.getAddress()) ? "  >>> " : "      ";
                emit(out, marker + cur.getAddress() + "  " + cur.toString());

                // any operand that lands inside a known function is a candidate implementation
                for (int op = 0; op < cur.getNumOperands(); op++) {
                    Object[] objs = cur.getOpObjects(op);
                    for (Object o : objs) {
                        if (o instanceof Scalar) {
                            long v = ((Scalar) o).getUnsignedValue();
                            if (v > 0x400000L && v < 0x2000000L) {
                                Address cand = addr(Long.toHexString(v));
                                Function cf = getFunctionAt(cand);
                                if (cf != null) {
                                    emit(out, "             ^ operand " + cand +
                                              " IS a function: " + cf.getName() +
                                              " (params=" + cf.getParameterCount() + ")");
                                }
                            }
                        }
                    }
                }
                cur = cur.getNext();
            }
        }

        // --- decompile the registration function if it is a sane size ---
        Address regAddr = addr("0090f770");
        Function reg = getFunctionContaining(regAddr);
        if (reg != null && reg.getBody().getNumAddresses() < 20000) {
            emit(out, "");
            emit(out, "==================================================");
            emit(out, " decompiled " + reg.getName());
            emit(out, "==================================================");
            DecompInterface di = new DecompInterface();
            di.openProgram(currentProgram);
            DecompileResults res = di.decompileFunction(reg, 120, monitor);
            if (res != null && res.decompileCompleted()) {
                emit(out, res.getDecompiledFunction().getC());
            } else {
                emit(out, "  (decompilation failed: " + (res != null ? res.getErrorMessage() : "null") + ")");
            }
            di.dispose();
        } else if (reg != null) {
            emit(out, "");
            emit(out, "FUN_0090f770 is " + reg.getBody().getNumAddresses() +
                      " bytes - too large to decompile usefully; it is a registration table.");
        }

        out.close();
        println("wrote " + outPath);
    }

    private void emit(PrintWriter out, String s) {
        out.println(s);
        println(s);
    }

    private Address addr(String hex) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace()
                .getAddress(Long.parseLong(hex, 16));
    }
}
