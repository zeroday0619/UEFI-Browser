#!/usr/bin/env python3
"""Serve only the isolated browser identity fixture's DNS names."""
import socket
import struct
server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
server.bind(('127.0.0.1', 15353))
while True:
    packet, peer = server.recvfrom(4096)
    if len(packet) < 17 or packet[4:6] != b'\x00\x01': continue
    position = 12
    labels = []
    while position < len(packet) and packet[position]:
        length = packet[position]
        if length > 63: break
        labels.append(packet[position+1:position+1+length].decode('ascii', errors='replace'))
        position += length + 1
    else:
        end = position + 5
        if end > len(packet): continue
        name = '.'.join(labels)
        kind = struct.unpack('!H', packet[position+1:position+3])[0]
        allowed = name in ('google.com', 'google.com.evil.test')
        answer = allowed and kind == 1
        header = packet[:2] + struct.pack('!HHHHH', 0x8180 if allowed else 0x8183, 1, int(answer), 0, 0)
        response = header + packet[12:end]
        if answer: response += b'\xc0\x0c' + struct.pack('!HHIH', 1, 1, 0, 4) + socket.inet_aton('10.0.2.2')
        server.sendto(response, peer)
        print(name, kind, answer, flush=True)
