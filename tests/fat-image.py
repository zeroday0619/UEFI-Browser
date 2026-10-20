#!/usr/bin/env python3
"""Create a dedicated FAT16 test disk or read a file from it without mounting."""
import argparse
from pathlib import Path
import struct

SECTOR=512
PARTITION=2048
TOTAL=1048576
CLUSTER_SECTORS=32
FAT_SECTORS=128
ROOT_SECTORS=32
DATA_SECTOR=1+2*FAT_SECTORS+ROOT_SECTORS
CLUSTER_BYTES=CLUSTER_SECTORS*SECTOR


def create(image, application):
    length=application.stat().st_size
    count=(length+CLUSTER_BYTES-1)//CLUSTER_BYTES
    if count+4 >= (TOTAL-DATA_SECTOR)//CLUSTER_SECTORS:
        raise ValueError('EFI application exceeds the test disk capacity')
    def entry(name, attributes, cluster, size=0):
        result=bytearray(32)
        result[:11]=name.encode('ascii').ljust(11,b' ')
        result[11]=attributes
        struct.pack_into('<H',result,26,cluster)
        struct.pack_into('<I',result,28,size)
        return result
    with image.open('xb') as output:
        output.truncate((PARTITION+TOTAL)*SECTOR)
        mbr=bytearray(SECTOR)
        mbr[446:462]=struct.pack('<B3sB3sII',0x80,b'\xfe\xff\xff',0x0e,b'\xfe\xff\xff',PARTITION,TOTAL)
        mbr[510:]=b'\x55\xaa'
        output.write(mbr)
        boot=bytearray(SECTOR)
        boot[:11]=b'\xeb\x3c\x90UEFITEST'
        struct.pack_into('<HBHBHHBHHHII',boot,11,512,32,1,2,512,0,0xf8,128,63,255,PARTITION,TOTAL)
        boot[36]=0x80;boot[38]=0x29
        struct.pack_into('<I',boot,39,0x55454649)
        boot[43:54]=b'UEFI TEST  ';boot[54:62]=b'FAT16   ';boot[510:]=b'\x55\xaa'
        output.seek(PARTITION*SECTOR);output.write(boot)
        fat=bytearray(FAT_SECTORS*SECTOR)
        for cluster,value in [(0,0xfff8),(1,0xffff),(2,0xffff),(3,0xffff)]:struct.pack_into('<H',fat,2*cluster,value)
        for cluster in range(4,4+count):struct.pack_into('<H',fat,2*cluster,0xffff if cluster==3+count else cluster+1)
        for copy in range(2):
            output.seek((PARTITION+1+copy*FAT_SECTORS)*SECTOR);output.write(fat)
        output.seek((PARTITION+1+2*FAT_SECTORS)*SECTOR);output.write(entry('EFI',16,2))
        output.seek((PARTITION+DATA_SECTOR)*SECTOR)
        output.write(entry('.',16,2)+entry('..',16,0)+entry('BOOT',16,3))
        output.seek((PARTITION+DATA_SECTOR+CLUSTER_SECTORS)*SECTOR)
        output.write(entry('.',16,3)+entry('..',16,2)+entry('BOOTX64 EFI',32,4,length))
        output.seek((PARTITION+DATA_SECTOR+2*CLUSTER_SECTORS)*SECTOR)
        with application.open('rb') as source:
            while data:=source.read(1024*1024):output.write(data)


def _read_file(image,path,byte_offset=None):
    with image.open('rb') as source:
        source.seek((PARTITION+1)*SECTOR);fat=source.read(FAT_SECTORS*SECTOR)
        def chain(cluster):
            output=bytearray();seen=set()
            while cluster<0xfff8:
                if cluster<2 or cluster in seen:raise ValueError('Invalid FAT chain')
                seen.add(cluster)
                source.seek((PARTITION+DATA_SECTOR+(cluster-2)*CLUSTER_SECTORS)*SECTOR)
                output.extend(source.read(CLUSTER_BYTES))
                cluster=struct.unpack_from('<H',fat,cluster*2)[0]
            return output
        source.seek((PARTITION+1+2*FAT_SECTORS)*SECTOR)
        directory=source.read(ROOT_SECTORS*SECTOR)
        for component in path.strip('/').split('/'):
            long_parts={};found=None
            for offset in range(0,len(directory),32):
                entry=directory[offset:offset+32]
                if not entry[0]:break
                if entry[0]==0xe5:long_parts={};continue
                if entry[11]==15:
                    text=entry[1:11]+entry[14:26]+entry[28:32]
                    long_parts[entry[0]&31]=text
                    continue
                name=entry[:8].decode('ascii').rstrip()
                extension=entry[8:11].decode('ascii').rstrip()
                if extension:name+='.'+extension
                if long_parts:
                    name=b''.join(long_parts[index] for index in sorted(long_parts)).decode('utf-16-le').split('\0')[0].rstrip('\uffff')
                long_parts={}
                if name.casefold()==component.casefold():
                    found=entry;break
            if found is None:raise FileNotFoundError(path)
            cluster=struct.unpack_from('<H',found,26)[0]
            length=struct.unpack_from('<I',found,28)[0]
            directory=chain(cluster) if cluster else bytearray()
        if byte_offset is not None:
            if found[11] & 16 or not 0 <= byte_offset < length:
                raise ValueError('File offset is outside the regular file')
            clusters, remainder = divmod(byte_offset, CLUSTER_BYTES)
            for _ in range(clusters):
                cluster = struct.unpack_from('<H', fat, cluster * 2)[0]
            return (PARTITION + DATA_SECTOR + (cluster - 2) * CLUSTER_SECTORS) * SECTOR + remainder
        return bytes(directory if found[11] & 16 else directory[:length])


def read_file(image, path):
    return _read_file(image, path)


def file_byte_offset(image, path, byte_offset):
    """Resolve a logical byte through the FAT chain, including fragmented files."""
    return _read_file(image, path, byte_offset)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image',type=Path)
    group=parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--application',type=Path)
    group.add_argument('--read')
    options=parser.parse_args()
    if options.application:create(options.image,options.application)
    else:print(read_file(options.image,options.read).hex())
