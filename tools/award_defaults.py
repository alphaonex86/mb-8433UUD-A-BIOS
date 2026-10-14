#!/usr/bin/env python3
"""
award_defaults.py: change setup defaults of the board's original Award 4.50PG BIOS (a dump of its 128 KiB chip).
Its setup items are 25-byte records in the LZH (-lh5-) compressed system BIOS module (original.tmp): label offset,
FFFF, mask, 00, CMOS index, mask, ..., then the BIOS and Setup default values at bytes 17 and 19. With a dead CMOS
battery the BIOS loads them at every boot, so the only way to keep a setting is to change them here.
  award_defaults.py IN.bin OUT.bin [--l2-off] [--boot-c] [--2mbit]
--l2-off: External Cache Disabled (CMOS 3Bh bit 0); --boot-c: Boot Sequence C,A (CMOS 3Ch bit 0);
--2mbit: OUT is 256 KiB for a 2 Mbit chip (lower half 0xFF). The module is recompressed by this script's own -lh5-
encoder and checked by its decoder; the module checksum byte and the image's byte sum are kept.
"""
import argparse
import heapq
import sys

DICBIT, MAXMATCH, THRESHOLD = 13, 256, 3
DICSIZ = 1 << DICBIT
NC, NP, NT, CBIT, PBIT, TBIT = 255 + MAXMATCH + 2 - THRESHOLD, DICBIT + 1, 19, 9, 4, 5


class BitReader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def bits(self, n):
        v = 0
        for _ in range(n):
            byte = self.data[self.pos >> 3] if (self.pos >> 3) < len(self.data) else 0
            v = v << 1 | (byte >> (7 - (self.pos & 7)) & 1)
            self.pos += 1
        return v


class BitWriter:
    def __init__(self):
        self.out, self.acc, self.n = bytearray(), 0, 0

    def put(self, n, v):
        for i in range(n - 1, -1, -1):
            self.acc = self.acc << 1 | (v >> i & 1)
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                self.acc, self.n = 0, 0

    def flush(self):
        if self.n:
            self.put(8 - self.n, 0)
        return bytes(self.out)


def canonical_codes(lengths):
    """LHA make_code: codes by length, then by symbol; a complete code is required."""
    count = [0] * 18
    for ln in lengths:
        count[ln] += 1
    start, code = [0] * 18, 0
    for i in range(1, 17):
        start[i] = code
        code = (code + count[i]) << 1
    if lengths and max(lengths) and code != 1 << 17:
        raise ValueError("incomplete Huffman code")
    codes = []
    for ln in lengths:
        codes.append(start[ln] if ln else 0)
        if ln:
            start[ln] += 1
    return codes


def decode_symbol(br, lengths, codes):
    code, ln = 0, 0
    while True:
        code = code << 1 | br.bits(1)
        ln += 1
        for s, (l2, c2) in enumerate(zip(lengths, codes)):
            if l2 == ln and c2 == code:
                return s
        if ln > 16:
            raise ValueError("bad code")


def read_pt_len(br, n, nbit, special):
    k = br.bits(nbit)
    if k == 0:
        return None, br.bits(nbit)
    lengths, i = [0] * n, 0
    while i < k:
        c = br.bits(3)
        if c == 7:
            while br.bits(1):
                c += 1
        lengths[i] = c
        i += 1
        if i == special:
            i += br.bits(2)
    return lengths, None


def lh5_decode(data, size):
    br, out = BitReader(data), bytearray()
    while len(out) < size:
        blocksize = br.bits(16)
        t_len, t_one = read_pt_len(br, NT, TBIT, 3)
        if t_len is None:
            c_one = br.bits(CBIT)
            c_len = None
        else:
            t_codes, n, i, c_len = canonical_codes(t_len), br.bits(CBIT), 0, [0] * NC
            while i < n:
                c = decode_symbol(br, t_len, t_codes)
                if c <= 2:
                    run = 1 if c == 0 else br.bits(4) + 3 if c == 1 else br.bits(CBIT) + 20
                    i += run
                else:
                    c_len[i] = c - 2
                    i += 1
            c_codes = canonical_codes(c_len)
        p_len, p_one = read_pt_len(br, NP, PBIT, -1)
        p_codes = canonical_codes(p_len) if p_len else None
        for _ in range(blocksize):
            c = c_one if c_len is None else decode_symbol(br, c_len, c_codes)
            if c < 256:
                out.append(c)
                continue
            p = p_one if p_len is None else decode_symbol(br, p_len, p_codes)
            dist = (1 << (p - 1)) + br.bits(p - 1) if p > 1 else p
            for _ in range(c - 253):
                out.append(out[-dist - 1])
    return bytes(out[:size])


def huffman_lengths(freq):
    """Huffman code lengths limited to 16 bits like LHA's make_len (the most frequent symbols get the short codes)."""
    used = [s for s, f in enumerate(freq) if f]
    if len(used) < 2:
        return None
    heap = [(freq[s], s, None) for s in used]
    heapq.heapify(heap)
    depth, uid, kids = {}, len(freq), {}
    while len(heap) > 1:
        f1, a, _ = heapq.heappop(heap)
        f2, b, _ = heapq.heappop(heap)
        kids[uid] = (a, b)
        heapq.heappush(heap, (f1 + f2, uid, None))
        uid += 1
    stack = [(heap[0][1], 0)]
    while stack:
        node, d = stack.pop()
        if node in kids:
            stack += [(kids[node][0], d + 1), (kids[node][1], d + 1)]
        else:
            depth[node] = d
    len_cnt = [0] * 17
    for d in depth.values():
        len_cnt[min(d, 16)] += 1
    cum = sum(len_cnt[i] << (16 - i) for i in range(1, 17))
    while cum != 1 << 16:
        len_cnt[16] -= 1
        for i in range(15, 0, -1):
            if len_cnt[i]:
                len_cnt[i] -= 1
                len_cnt[i + 1] += 2
                break
        cum -= 1
    lengths, order, k = [0] * len(freq), sorted(used, key=lambda s: (-freq[s], s)), 0
    for ln in range(1, 17):
        for _ in range(len_cnt[ln]):
            lengths[order[k]] = ln
            k += 1
    return lengths


def lz_tokens(data):
    """Greedy LZ77 with one-step lazy matching over an 8 KiB window: literals and (length, distance - 1)."""
    chains, tokens, i = {}, [], 0

    def longest(pos):
        best_len, best_dist = 0, 0
        for cand in reversed(chains.get(data[pos:pos + 3], [])[-64:]):
            if pos - cand > DICSIZ - 1:
                break
            ln = 0
            while ln < MAXMATCH and pos + ln < len(data) and data[cand + ln] == data[pos + ln]:
                ln += 1
            if ln > best_len:
                best_len, best_dist = ln, pos - cand
                if ln == MAXMATCH:
                    break
        return best_len, best_dist

    def insert(pos):
        if pos + 3 <= len(data):
            chains.setdefault(data[pos:pos + 3], []).append(pos)

    while i < len(data):
        ln, dist = longest(i) if i + 3 <= len(data) else (0, 0)
        if ln >= THRESHOLD and i + 1 + 3 <= len(data):
            insert(i)
            nxt, _ = longest(i + 1)
            if nxt > ln:
                tokens.append(data[i])
                i += 1
                continue
            for p in range(i + 1, i + ln):
                insert(p)
            tokens.append((ln, dist - 1))
            i += ln
        elif ln >= THRESHOLD:
            for p in range(i, i + ln):
                insert(p)
            tokens.append((ln, dist - 1))
            i += ln
        else:
            insert(i)
            tokens.append(data[i])
            i += 1
    return tokens


def write_pt_len(bw, lengths, nbit, special):
    n = len(lengths)
    while n and lengths[n - 1] == 0:
        n -= 1
    bw.put(nbit, n)
    i = 0
    while i < n:
        k = lengths[i]
        i += 1
        bw.put(3, k) if k <= 6 else bw.put(k - 3, (1 << (k - 3)) - 2)
        if i == special:
            while i < 6 and lengths[i] == 0:
                i += 1
            bw.put(2, (i - 3) & 3)


def c_len_runs(c_len):
    """write_c_len's symbols: (t symbol, extra bits count, extra value)."""
    n = len(c_len)
    while n and c_len[n - 1] == 0:
        n -= 1
    out, i = [], 0
    while i < n:
        k = c_len[i]
        i += 1
        if k:
            out.append((k + 2, 0, 0))
            continue
        count = 1
        while i < n and c_len[i] == 0:
            i += 1
            count += 1
        if count <= 2:
            out += [(0, 0, 0)] * count
        elif count <= 18:
            out.append((1, 4, count - 3))
        elif count == 19:
            out += [(0, 0, 0), (1, 4, 15)]
        else:
            out.append((2, CBIT, count - 20))
    return n, out


def p_symbol(p):
    return p.bit_length()


def lh5_encode(data):
    bw, tokens = BitWriter(), lz_tokens(data)
    for start in range(0, len(tokens), 0x4000):
        block = tokens[start:start + 0x4000]
        c_freq, p_freq = [0] * NC, [0] * NP
        for t in block:
            if isinstance(t, int):
                c_freq[t] += 1
            else:
                c_freq[t[0] + 253] += 1
                p_freq[p_symbol(t[1])] += 1
        bw.put(16, len(block))
        c_len = huffman_lengths(c_freq)
        if c_len is None:
            bw.put(TBIT, 0), bw.put(TBIT, 0), bw.put(CBIT, 0), bw.put(CBIT, c_freq.index(max(c_freq)))
            c_codes = None
        else:
            n, runs = c_len_runs(c_len)
            t_freq = [0] * NT
            for sym, _, _ in runs:
                t_freq[sym] += 1
            t_len = huffman_lengths(t_freq)
            if t_len is None:
                bw.put(TBIT, 0), bw.put(TBIT, t_freq.index(max(t_freq)))
            else:
                write_pt_len(bw, t_len, TBIT, 3)
            t_codes = canonical_codes(t_len) if t_len else None
            bw.put(CBIT, n)
            for sym, nbits, val in runs:
                if t_len:
                    bw.put(t_len[sym], t_codes[sym])
                if nbits:
                    bw.put(nbits, val)
            c_codes = canonical_codes(c_len)
        p_len = huffman_lengths(p_freq)
        if p_len is None:
            bw.put(PBIT, 0), bw.put(PBIT, p_freq.index(max(p_freq)) if any(p_freq) else 0)
        else:
            write_pt_len(bw, p_len, PBIT, -1)
        p_codes = canonical_codes(p_len) if p_len else None
        for t in block:
            c = t if isinstance(t, int) else t[0] + 253
            if c_codes:
                bw.put(c_len[c], c_codes[c])
            if isinstance(t, int):
                continue
            p = p_symbol(t[1])
            if p_codes:
                bw.put(p_len[p], p_codes[p])
            if p > 1:
                bw.put(p - 1, t[1] & ((1 << (p - 1)) - 1))
    return bw.flush()


def crc16(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xa001 if crc & 1 else crc >> 1
    return crc


def find_item(module, label, cmos, mask):
    """The setup item record whose label text starts with 'label' (module offsets are segment offsets)."""
    text = module.find(label)
    if text < 0:
        sys.exit(f"award_defaults: no '{label.decode()}' label")
    ref = (text & 0xffff).to_bytes(2, "little") + b"\xff\xff" + bytes([mask, 0, cmos, mask])
    at = module.find(ref)
    if at < 0 or module.find(ref, at + 1) >= 0:
        sys.exit(f"award_defaults: no unique setup record for '{label.decode()}'")
    return at


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inp")
    ap.add_argument("out")
    ap.add_argument("--l2-off", action="store_true")
    ap.add_argument("--boot-c", action="store_true")
    ap.add_argument("--2mbit", dest="mbit2", action="store_true")
    args = ap.parse_args()
    rom = bytearray(open(args.inp, "rb").read())
    if len(rom) != 0x20000 or rom[2:7] != b"-lh5-":
        sys.exit("award_defaults: not the 128 KiB Award image with original.tmp first")
    hsize, comp, orig = rom[0], int.from_bytes(rom[7:11], "little"), int.from_bytes(rom[11:15], "little")
    data_at = 2 + hsize
    module = bytearray(lh5_decode(rom[data_at:data_at + comp], orig))
    name_len = rom[21]
    if crc16(module) != int.from_bytes(rom[22 + name_len:24 + name_len], "little"):
        sys.exit("award_defaults: the decoder disagrees with the module CRC")
    mod_end = data_at + comp
    if rom[mod_end] != 0 or rom[mod_end + 1] != sum(rom[:mod_end]) & 0xff:
        sys.exit("award_defaults: unexpected bytes after original.tmp (end marker, module checksum)")
    nxt = mod_end + 2
    nxt_end = nxt + 2 + rom[nxt] + int.from_bytes(rom[nxt + 7:nxt + 11], "little") + 1   # its header, data, 00
    free_end = nxt_end
    while free_end < len(rom) and rom[free_end] == 0xff:
        free_end += 1
    changes = []
    if args.l2_off:
        changes.append((b"External Cache ", 0x3b, 0x01, 0x00))
    if args.boot_c:
        changes.append((b"Boot Sequence ", 0x3c, 0x01, 0x01))
    for label, cmos, mask, value in changes:
        at = find_item(module, label, cmos, mask)
        module[at + 17] = module[at + 19] = value
        print(f"{label.decode().strip()}: CMOS {cmos:02X}h mask {mask:02X}, defaults -> {value:02X}")
    packed = lh5_encode(bytes(module))
    if lh5_decode(packed, orig) != bytes(module):
        sys.exit("award_defaults: the encoder output does not decode back")
    header = bytearray(rom[:data_at])
    header[7:11] = len(packed).to_bytes(4, "little")
    header[22 + name_len:24 + name_len] = crc16(module).to_bytes(2, "little")
    header[1] = sum(header[2:]) & 0xff
    first = bytes(header) + packed + b"\x00"
    rest = bytes(rom[nxt:nxt_end])
    area = first + bytes([sum(first[:-1]) & 0xff]) + rest
    if len(area) >= free_end:
        sys.exit("award_defaults: the recompressed modules do not fit before the boot code")
    new = bytearray(area + b"\xff" * (free_end - len(area))) + rom[free_end:]
    new[free_end - 1] = (new[free_end - 1] - (sum(new) - sum(rom))) & 0xff   # the image's byte sum unchanged
    print(f"original.tmp: {comp} -> {len(packed)} bytes; modules end at {len(area):05x}, free until {free_end:05x}")
    if args.mbit2:
        new = bytearray(b"\xff" * 0x20000) + new
    open(args.out, "wb").write(new)


if __name__ == "__main__":
    main()
