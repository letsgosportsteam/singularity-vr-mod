// FindProcessEvent - second pass at locating UObject::ProcessEvent.
//
// Pass 1 (FindScriptVM) found execUndefined at 0049f870 and the native dispatch tables that
// reference it, but not ProcessEvent itself. UE3's ProcessEvent carries a distinctive
// recursion guard whose message contains the word "recursion" - there is exactly one such
// UTF-16 string in this binary. Resolve it, read its full text, and follow its xrefs.
//
// Also dumps the native dispatch table around a known execUndefined slot, since that table
// is independently useful (it maps opcodes to native handlers).
//
// Run:  .\tools\ghidra.ps1 -Script FindProcessEvent.java
//@category Singularity

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

public class FindProcessEvent extends GhidraScript {

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter("R:\\SingularityVR-Dev\\ghidra_projects\\process_event.txt");
        Memory mem = currentProgram.getMemory();

        // ---- 1. the "recursion" string, whatever it actually says ----
        emit(out, "=== hunting the 'recursion' string ===");
        for (Address h : findUtf16(mem, "recursion")) {
            String full = readWideAround(mem, h);
            emit(out, "  @ " + h + "  -> \"" + full + "\"");
            ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(h);
            boolean any = false;
            while (it.hasNext()) {
                Reference r = it.next();
                any = true;
                Function f = getFunctionContaining(r.getFromAddress());
                emit(out, "    xref " + r.getFromAddress() +
                          (f != null ? ("  in " + f.getName() + " entry=" + f.getEntryPoint()
                                        + " size=" + f.getBody().getNumAddresses()
                                        + " params=" + f.getParameterCount())
                                     : "  (no function)"));
                if (f != null) decompile(out, f, 20000);
            }
            if (!any) emit(out, "    (no references)");
        }

        // ---- 2. what is the table at 0x0172xxxx that points at execUndefined? ----
        emit(out, "");
        emit(out, "=== native dispatch table around 01721877 ===");
        Address tbl = addr("01721860");
        for (int i = 0; i < 24; i++) {
            Address slot = tbl.add(i * 4L);
            try {
                long v = mem.getInt(slot) & 0xFFFFFFFFL;
                String note = "";
                if (v > 0x400000L && v < 0x2000000L) {
                    Function f = getFunctionAt(addr(Long.toHexString(v)));
                    if (f != null) note = "  -> " + f.getName() + " (size " + f.getBody().getNumAddresses() + ")";
                }
                emit(out, String.format("  %s : %08X%s", slot, v, note));
            } catch (Exception e) { emit(out, "  " + slot + " : <unreadable>"); }
        }

        // ---- 3. the two direct callers of execUndefined ----
        emit(out, "");
        emit(out, "=== direct callers of execUndefined ===");
        for (String a : new String[] { "004a1930", "004a1990" }) {
            Function f = getFunctionAt(addr(a));
            if (f != null) { emit(out, ""); emit(out, "--- " + f.getName() + " ---"); decompile(out, f, 20000); }
        }

        out.close();
        println("wrote R:\\SingularityVR-Dev\\ghidra_projects\\process_event.txt");
    }

    private void decompile(PrintWriter out, Function f, int maxSize) {
        if (f.getBody().getNumAddresses() > maxSize) {
            emit(out, "      (function is " + f.getBody().getNumAddresses() + " bytes - too big, skipping)");
            return;
        }
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        DecompileResults res = di.decompileFunction(f, 90, monitor);
        if (res != null && res.decompileCompleted()) emit(out, res.getDecompiledFunction().getC());
        else emit(out, "      (decompile failed)");
        di.dispose();
    }

    private String readWideAround(Memory mem, Address hit) {
        // walk back to the start of the wide string, then read it out
        Address start = hit;
        try {
            for (int i = 0; i < 200; i++) {
                Address prev = start.subtract(2);
                if (mem.getShort(prev) == 0) break;
                start = prev;
            }
            StringBuilder sb = new StringBuilder();
            Address at = start;
            for (int i = 0; i < 300; i++) {
                short c = mem.getShort(at);
                if (c == 0) break;
                sb.append((char) c);
                at = at.add(2);
            }
            return sb.toString();
        } catch (Exception e) { return "<unreadable>"; }
    }

    private void emit(PrintWriter out, String s) { out.println(s); println(s); }

    private List<Address> findUtf16(Memory mem, String s) throws Exception {
        byte[] pat = new byte[s.length() * 2];
        for (int i = 0; i < s.length(); i++) { pat[i*2] = (byte) s.charAt(i); pat[i*2+1] = 0; }
        List<Address> out = new ArrayList<>();
        for (MemoryBlock blk : mem.getBlocks()) {
            if (!blk.isInitialized()) continue;
            Address at = blk.getStart();
            while (at != null && at.compareTo(blk.getEnd()) < 0) {
                Address f = mem.findBytes(at, blk.getEnd(), pat, null, true, monitor);
                if (f == null) break;
                out.add(f);
                at = f.add(2);
                if (out.size() > 20) return out;
            }
        }
        return out;
    }

    private Address addr(String hex) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace()
                .getAddress(Long.parseLong(hex, 16));
    }
}
