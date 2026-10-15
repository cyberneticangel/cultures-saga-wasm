"""PE image loader: maps sections into a flat bytearray for the analyzer/recompiler."""
import pefile, struct

class Image:
    def __init__(self, path):
        pe = pefile.PE(path)
        self.pe = pe
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.size = pe.OPTIONAL_HEADER.SizeOfImage
        self.entry = self.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.mem = bytearray(self.size)
        hdr = pe.OPTIONAL_HEADER.SizeOfHeaders
        self.mem[:hdr] = pe.__data__[:hdr]
        self.sections = []
        for s in pe.sections:
            name = s.Name.rstrip(b'\0').decode()
            raw = s.get_data()[:max(s.Misc_VirtualSize, 0) or s.SizeOfRawData]
            self.mem[s.VirtualAddress:s.VirtualAddress + len(raw)] = raw
            self.sections.append((name, self.base + s.VirtualAddress,
                                  self.base + s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData)))
        self.text = next((a, b) for n, a, b in self.sections if n == '.text')
        # imports: IAT slot address -> (dll, name)
        self.imports = {}
        for e in pe.DIRECTORY_ENTRY_IMPORT:
            for i in e.imports:
                name = i.name.decode() if i.name else 'ord%d' % i.ordinal
                self.imports[i.address] = (e.dll.decode().upper(), name)

    def in_text(self, a):
        return self.text[0] <= a < self.text[1]

    def valid(self, a):
        return self.base <= a < self.base + self.size

    def u32(self, a):
        return struct.unpack_from('<I', self.mem, a - self.base)[0]

    def u8(self, a):
        return self.mem[a - self.base]

    def bytes(self, a, n):
        o = a - self.base
        return bytes(self.mem[o:o + n])
