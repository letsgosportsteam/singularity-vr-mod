// FindProcessInternal - locate ProcessInternal and then ProcessEvent via GNatives.
//
// GRegisterNative was identified at 0x004a1990 (it fills 255 slots with execUndefined then
// assigns GNatives[iNative]). That pins GNatives at 0x01cc8330. Only the bytecode
// interpreter dispatches THROUGH that table, so any code that reads GNatives and calls
// through it is ProcessInternal (or CallFunction). ProcessEvent is then among its callers.
//
// Run:  .\tools\ghidra.ps1 -Script FindProcessInternal.java
//@category Singularity

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.io.PrintWriter;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;

public class FindProcessInternal extends GhidraScript {

    private static final long GNATIVES = 0x01cc8330L;

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter("R:\\SingularityVR-Dev\\ghidra_projects\\process_internal.txt");

        emit(out, "=== scanning all instructions for the GNatives base 0x" +
                  Long.toHexString(GNATIVES) + " ===");

        // Map function -> how many times it touches GNatives, and whether it CALLs.
        Map<Function, int[]> touch = new LinkedHashMap<>();   // [refs, calls]
        Set<String> sites = new LinkedHashSet<>();

        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        while (it.hasNext()) {
            if (monitor.isCancelled()) break;
            Instruction ins = it.next();
            boolean hits = false;
            for (int op = 0; op < ins.getNumOperands(); op++) {
                for (Object o : ins.getOpObjects(op)) {
                    if (o instanceof Scalar && ((Scalar) o).getUnsignedValue() == GNATIVES) hits = true;
                    if (o instanceof Address && ((Address) o).getOffset() == GNATIVES) hits = true;
                }
            }
            if (!hits) continue;

            Function f = getFunctionContaining(ins.getAddress());
            String mn = ins.getMnemonicString().toUpperCase();
            boolean isCall = mn.startsWith("CALL");
            sites.add("  " + ins.getAddress() + "  " + ins +
                      (f != null ? ("   [" + f.getName() + " size=" + f.getBody().getNumAddresses() + "]") : ""));
            if (f != null) {
                int[] c = touch.computeIfAbsent(f, k -> new int[2]);
                c[0]++;
                if (isCall) c[1]++;
            }
        }

        for (String s : sites) emit(out, s);

        emit(out, "");
        emit(out, "=== functions touching GNatives, ranked ===");
        touch.entrySet().stream()
            .sorted((a, b) -> Long.compare(b.getKey().getBody().getNumAddresses(),
                                           a.getKey().getBody().getNumAddresses()))
            .forEach(e -> {
                Function f = e.getKey();
                emit(out, String.format("  %-16s entry=%s size=%-7d refs=%d calls=%d",
                        f.getName(), f.getEntryPoint(), f.getBody().getNumAddresses(),
                        e.getValue()[0], e.getValue()[1]));
            });

        // The interpreter is the large one. List its callers - ProcessEvent lives there.
        emit(out, "");
        emit(out, "=== callers of each candidate (ProcessEvent should be among them) ===");
        for (Function f : touch.keySet()) {
            if (f.getBody().getNumAddresses() < 100) continue;   // skip the registrar thunks
            emit(out, "");
            emit(out, "--- callers of " + f.getName() + " (size " + f.getBody().getNumAddresses() + ") ---");
            ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
            int n = 0;
            Set<String> seen = new LinkedHashSet<>();
            while (ri.hasNext() && n < 25) {
                Reference r = ri.next();
                Function cf = getFunctionContaining(r.getFromAddress());
                String line = (cf != null)
                    ? String.format("  %-16s entry=%s size=%-7d params=%d",
                        cf.getName(), cf.getEntryPoint(), cf.getBody().getNumAddresses(), cf.getParameterCount())
                    : "  (indirect from " + r.getFromAddress() + ")";
                if (seen.add(line)) { emit(out, line); n++; }
            }
            if (n == 0) emit(out, "  (none direct - reached via vtable)");
        }

        out.close();
        println("wrote R:\\SingularityVR-Dev\\ghidra_projects\\process_internal.txt");
    }

    private void emit(PrintWriter out, String s) { out.println(s); println(s); }
}
