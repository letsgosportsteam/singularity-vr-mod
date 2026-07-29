// FindCameraUpdate - find RvPlayerCamera's vtable and its update function.
//
// Memory hunting found where pitch ENDS UP (controller +0x60/+0x5D4, camera +0x60, the POV
// copies) but not where the camera READS it from during gameplay. Writes to every one of
// those persist yet never tilt the view in gameplay - though they DO work in the menu, where
// the camera stops updating. So the camera recomputes pitch each frame from an upstream
// source we have not located.
//
// FUN_00f24770 (153 bytes) references the "RvPlayerCamera" string and is the right shape for
// UE3's IMPLEMENT_CLASS registrant, which carries the vtable pointer. From the vtable we can
// reach the camera's virtual update and read what it actually uses for pitch.
//
// Run:  .\tools\ghidra.ps1 -Script FindCameraUpdate.java
//@category Singularity

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.scalar.Scalar;

import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

public class FindCameraUpdate extends GhidraScript {

    private static final String REGISTRANT = "00f24770";   // references "RvPlayerCamera"

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter("R:\\SingularityVR-Dev\\ghidra_projects\\camera_update.txt");

        Function reg = getFunctionContaining(addr(REGISTRANT));
        if (reg == null) { emit(out, "no function at " + REGISTRANT); out.close(); return; }

        emit(out, "=== class registrant " + reg.getName() + " ===");
        decompile(out, reg);

        // Any constant it pushes that points into .rdata is a vtable candidate.
        emit(out, "");
        emit(out, "=== pointer-sized constants referenced by the registrant ===");
        Set<Long> cands = new LinkedHashSet<>();
        Instruction ins = currentProgram.getListing().getInstructionAt(reg.getEntryPoint());
        int guard = 0;
        while (ins != null && reg.getBody().contains(ins.getAddress()) && guard++ < 400) {
            for (int op = 0; op < ins.getNumOperands(); op++) {
                for (Object o : ins.getOpObjects(op)) {
                    long v = -1;
                    if (o instanceof Scalar) v = ((Scalar) o).getUnsignedValue();
                    else if (o instanceof Address) v = ((Address) o).getOffset();
                    if (v > 0x400000L && v < 0x2000000L) cands.add(v);
                }
            }
            ins = ins.getNext();
        }
        for (long v : cands) {
            Address a = addr(Long.toHexString(v));
            String note = "";
            Function f = getFunctionAt(a);
            if (f != null) note = " -> function " + f.getName();
            else {
                // does it look like a vtable? i.e. a run of pointers into .text
                int good = 0;
                try {
                    for (int i = 0; i < 12; i++) {
                        long p = currentProgram.getMemory().getInt(a.add(i * 4L)) & 0xFFFFFFFFL;
                        if (p > 0x401000L && p < 0x1F53ABEL &&
                            getFunctionAt(addr(Long.toHexString(p))) != null) good++;
                    }
                } catch (Exception e) { }
                if (good >= 8) note = " -> VTABLE (" + good + "/12 slots are functions)";
                else if (good > 0) note = " -> " + good + "/12 slots look like functions";
            }
            emit(out, String.format("  %08X%s", v, note));
        }

        // For every vtable found, list its slots so the update/tick function can be spotted.
        emit(out, "");
        emit(out, "=== vtable contents (first 48 slots) ===");
        for (long v : cands) {
            Address a = addr(Long.toHexString(v));
            int good = 0;
            try {
                for (int i = 0; i < 12; i++) {
                    long p = currentProgram.getMemory().getInt(a.add(i * 4L)) & 0xFFFFFFFFL;
                    if (getFunctionAt(addr(Long.toHexString(p))) != null) good++;
                }
            } catch (Exception e) { continue; }
            if (good < 8) continue;
            emit(out, "");
            emit(out, "  --- vtable @ " + a + " ---");
            for (int i = 0; i < 48; i++) {
                try {
                    long p = currentProgram.getMemory().getInt(a.add(i * 4L)) & 0xFFFFFFFFL;
                    Function f = getFunctionAt(addr(Long.toHexString(p)));
                    emit(out, String.format("     [%2d] %08X  %s", i, p,
                            f != null ? (f.getName() + "  size=" + f.getBody().getNumAddresses()) : "?"));
                } catch (Exception e) { break; }
            }
        }

        out.close();
        println("wrote R:\\SingularityVR-Dev\\ghidra_projects\\camera_update.txt");
    }

    private void decompile(PrintWriter out, Function f) {
        if (f.getBody().getNumAddresses() > 20000) { emit(out, "  (too large)"); return; }
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        DecompileResults r = di.decompileFunction(f, 120, monitor);
        emit(out, (r != null && r.decompileCompleted()) ? r.getDecompiledFunction().getC() : "  (failed)");
        di.dispose();
    }

    private void emit(PrintWriter out, String s) { out.println(s); println(s); }

    private Address addr(String hex) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace()
                .getAddress(Long.parseLong(hex, 16));
    }
}
