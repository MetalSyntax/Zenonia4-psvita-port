import struct
import sys

def parse_psp2dmp(filepath):
    # psp2dmp is a gzip file. we can just gunzip it.
    import gzip
    try:
        with gzip.open(filepath, 'rb') as f:
            data = f.read()
    except Exception as e:
        print("Error reading gzip:", e)
        return

    # ELF header check
    if data[0:4] != b'\x7fELF':
        print("Not an ELF core dump")
        return
        
    print("ELF core dump found. Size:", len(data))
    
    # Very basic search for PRSTATUS notes in ELF core
    # A PRSTATUS note has the registers. On ARM, r15 is PC, r14 is LR.
    # To keep it simple, let's just find the first thread context.
    # In ARM ELF core, PRSTATUS note type is 1.
    
    # We will just print some basic strings from the crash stack if possible
    # But let's look for "DrawOP" to see if it's in the stack.
    
    import re
    # Extract all printable strings > 4 chars
    strings = re.findall(b'[ -~]{5,}', data)
    func_names = [s.decode('ascii') for s in strings if b'DrawOP' in s or b'Vita' in s]
    print("Found symbols in dump related to our code:", func_names[:10])

parse_psp2dmp(sys.argv[1])
