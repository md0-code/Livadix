"""Original guest API probe for G3 boot validation.

Inside Macintosh: Operating System Utilities, Gestalt; Memory, NewPtr;
OS Utilities, ReadXPRAM/WriteXPRAM register interfaces. Runs as a normal
Startup Items application. It changes only a disposable test PRAM record
when write_pram is requested; it never patches guest or ROM code.
"""
import struct
SELECTORS=('mach','sysv','sysa','pclk')
TAG=0x47334750
SIZE=64
PRAM_PATTERN=b'G3PRAMOK'

def program(write_pram=False, pram_setting=None, serial_probe=b"", *, sound_alert=False):
    # NewPtrSysClear(128), preserve result base in A3, cursor in A2.
    out=bytearray.fromhex('203c00000080a71e26482448588a')
    for selector in SELECTORS:
        out+=bytes.fromhex('203c')+selector.encode('ascii')
        out+=bytes.fromhex('a1ad24c848c024c0')
    if pram_setting is not None:
        if not 0 <= pram_setting <= 3:
            raise ValueError('menu blink count must be 0..3')
        # Inside Macintosh: OS Utilities, Parameter RAM Utilities 7-27,
        # 7-31 and 7-35: documented SPMisc2 bits 3..2, SysParam $1F8.
        # WriteParam takes A0=SysParam and D0=MinusOne, returns D0=OSErr.
        if write_pram:
            out+=bytes.fromhex('3038020a0240fff30040')+struct.pack('>H',pram_setting << 2)
            out+=bytes.fromhex('31c0020a41f801f870ffa03848c027400038')
        out+=bytes.fromhex('70003038020a02800000000c27400024')
    else:
        if write_pram:
            out+=bytes.fromhex('277c')+PRAM_PATTERN[:4]+bytes.fromhex('0030')
            out+=bytes.fromhex('277c')+PRAM_PATTERN[4:]+bytes.fromhex('0034')
            out+=bytes.fromhex('41eb0030203c000800e0a05248c027400038')
        out+=bytes.fromhex('41eb0028203c000800e0a05148c02740003c')
    out+=serial_probe
    if sound_alert:
        out+=bytes.fromhex('3f3c000aa9c8')  # SysBeep(10)
    # Publish only after every API returned, then ExitToShell.
    out+=bytes.fromhex('26bc')+struct.pack('>I',TAG)+bytes.fromhex('a9f4')
    return bytes(out)

def decode(data):
    if len(data)<SIZE or int.from_bytes(data[:4],'big')!=TAG:
        raise AssertionError('no completed G3 guest API probe')
    result={}
    for i,selector in enumerate(SELECTORS):
        value,error=struct.unpack_from('>Ii',data,4+8*i)
        result[selector]={'value':value,'error':error}
    result['pram_setting']=(struct.unpack_from('>I',data,36)[0] >> 2) & 3
    result['pram']=data[40:48].hex()
    result['pram_write_error'],result['pram_read_error']=struct.unpack_from('>ii',data,56)
    return result

def serial_result(data):
    if len(data)<SIZE or int.from_bytes(data[:4],'big')!=TAG:
        raise AssertionError('no completed PBX serial probe')
    rxerr, rxlen, txerr, txlen, outerr, inerr = struct.unpack_from('>hHhHhh', data, 44)
    return data[40:44], rxerr, rxlen, txerr, txlen, outerr, inerr
