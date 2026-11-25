#!/usr/bin/env python3
"""Writes an empty HFS volume image for the boot matrix (tests/verify.py):

    python tests/hfs_blank.py FILE KIB [NAME]

KIB is the size in KiB (800 and 1440 make floppy images; any multiple of 2
also serves as a CD image of 2048-byte sectors when it is a multiple of 2).

The layout follows Inside Macintosh IV, "The File Manager", "Data
Organization on Volumes": boot blocks 0-1, the volume information (master
directory block) at block 2 with the fields and offsets of Figure 11, the
volume bit map from block 3, then allocation blocks holding the extents tree
and catalog tree files, B*-tree nodes as in Figure 15 (node descriptor,
records, offsets from the node's end), catalog keys (Figure 18), the root
directory record (Figure 20) and its thread record (Figure 21). The B*-tree
header node, which IV-169 names without giving its layout, is laid out as
Apple's own initialisation writes it, read from the volumes in data/disk:
node type 1 with three records (the header record: depth, root, leaf
records, first and last leaf, node size, maximum key length, node count, free
nodes; 128 reserved bytes; the node map, one bit per node), the node
descriptor's reserved word after ndNRecs (records start at offset 14), and an alternate
volume information block in the volume's last block but one.
"""
import struct
import sys
import time

BLOCK = 512
MAC_EPOCH = 2082844800


def node(kind, level, records, flink=0, blink=0):
    """A 512-byte B*-tree node (IM IV Figure 15)."""
    # The descriptor is 14 bytes: Figure 15's fields and a reserved word
    # (the first record starts at offset 14 in Apple's volumes).
    body = struct.pack('>IIbBHH', flink, blink, kind, level, len(records), 0)
    offsets = []
    for r in records:
        offsets.append(len(body))
        body += r
    offsets.append(len(body))
    tail = b''.join(struct.pack('>H', o) for o in reversed(offsets))
    assert len(body) + len(tail) <= BLOCK
    return body + bytes(BLOCK - len(body) - len(tail)) + tail


def header_node(depth, root, nrecs, first, last, keylen, nnodes, used):
    record = struct.pack('>HIIIIHHII', depth, root, nrecs, first, last, BLOCK, keylen, nnodes, nnodes - used)
    record += bytes(106 - len(record))
    bitmap = bytearray(256)
    for n in range(used):
        bitmap[n // 8] |= 0x80 >> (n % 8)
    return node(1, 0, [record, bytes(128), bytes(bitmap)])


def catalog_key(parent, name):
    """IM IV Figure 18; the record that follows starts on a word boundary."""
    key = struct.pack('>BI', 0, parent) + bytes([len(name)]) + name
    key = bytes([len(key)]) + key
    return key + (b'\0' if len(key) % 2 else b'')


def make(path, kib, name):
    name = name.encode('mac_roman')[:27]
    blocks = kib * 1024 // BLOCK
    now = int(time.time()) + MAC_EPOCH
    bitmap_blocks = (blocks + BLOCK * 8 - 1) // (BLOCK * 8)
    first_alloc = 3 + bitmap_blocks                  # drAlBlSt
    alloc_blocks = blocks - first_alloc - 2          # the last two blocks: alternate MDB, reserved
    tree = 4                                         # allocation blocks for each tree (one 512-byte node each)
    xt_start, ct_start = 0, tree
    used = 2 * tree
    img = bytearray(blocks * BLOCK)

    def at(ab):
        return (first_alloc + ab) * BLOCK

    # Extents tree: a header node only.
    img[at(xt_start):at(xt_start) + BLOCK] = header_node(0, 0, 0, 0, 0, 7, tree, 1)
    # Catalog tree: the header node and one leaf with the root directory
    # (ID 2, parent 1) and its thread (key: parent 2, empty name).
    root = catalog_key(1, name) + struct.pack('>BBHHIIII', 1, 0, 0, 0, 2, now, now, 0) + bytes(48)
    thread = catalog_key(2, b'') + struct.pack('>BB8sI', 3, 0, bytes(8), 1) + bytes([len(name)]) + name.ljust(31, b'\0')
    img[at(ct_start):at(ct_start) + BLOCK] = header_node(1, 1, 2, 1, 1, 37, tree, 2)
    img[at(ct_start) + BLOCK:at(ct_start) + 2 * BLOCK] = node(-1, 1, [root, thread])
    # Volume bit map (IV-167): one bit per allocation block in use.
    for b in range(used):
        img[3 * BLOCK + b // 8] |= 0x80 >> (b % 8)
    # Volume information (IM IV Figure 11).
    mdb = struct.pack('>HIIHHHHHIIHIH', 0x4244, now, now, 0x0100, 0, 3, used, alloc_blocks, BLOCK, 4 * BLOCK,
                      first_alloc, 16, alloc_blocks - used)
    mdb += bytes([len(name)]) + name.ljust(27, b'\0')
    mdb += struct.pack('>IHI', 0, 0, 0)               # drVolBkUp, drVSeqNum, drWrCnt
    mdb += struct.pack('>II', tree * BLOCK, tree * BLOCK)   # drXTClpSiz, drCTClpSiz
    mdb += struct.pack('>HII', 0, 0, 0)               # drNmRtDirs, drFilCnt, drDirCnt
    mdb += bytes(32)                                  # drFndrInfo
    mdb += struct.pack('>HHH', 0, 0, 0)               # drVCSize, drVBMCSize, drCtlCSize
    mdb += struct.pack('>I', tree * BLOCK) + struct.pack('>6H', xt_start, tree, 0, 0, 0, 0)
    mdb += struct.pack('>I', tree * BLOCK) + struct.pack('>6H', ct_start, tree, 0, 0, 0, 0)
    assert len(mdb) == 162
    img[2 * BLOCK:2 * BLOCK + len(mdb)] = mdb
    img[(blocks - 2) * BLOCK:(blocks - 2) * BLOCK + len(mdb)] = mdb
    with open(path, 'wb') as f:
        f.write(img)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    make(argv[0], int(argv[1]), argv[2] if len(argv) > 2 else 'Livadix Test')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
