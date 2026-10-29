"""Untrusted transport metadata only; native selected-chain validation is mandatory."""
import struct


def inspect(path):
    with open(path, 'rb') as stream:
        header = stream.read(68)
        if len(header) != 68 or header[:4] != b'UXTO':
            raise ValueError('invalid snapshot header')
        version = struct.unpack_from('<I', header, 4)[0]
        height = struct.unpack_from('<I', header, 40)[0]
        if version not in (4, 5, 6):
            raise ValueError('unsupported snapshot version')
        if height >= 150000 and version != 6:
            raise ValueError('Orchard base requires v6')
        if version == 6 and height < 150000:
            raise ValueError('v6 requires Orchard activation')
        if 111000 <= height < 150000 and version != 5:
            raise ValueError('base requires v5')
        profile = 'legacy'
        if version == 6:
            if struct.unpack_from('<Q', header, 60)[0] != 0:
                raise ValueError('nonzero reserved metadata')
            count = struct.unpack_from('<Q', header, 44)[0]
            # Bounded seeks through full coin records; never allocate from a file length.
            size = stream.seek(0, 2)
            stream.seek(68)
            if count > max(0, size - 108) // 53:
                raise ValueError('invalid coin count')
            for _ in range(count):
                coin = stream.read(48)
                if len(coin) != 48:
                    raise ValueError('truncated coin')
                length = struct.unpack_from('<I', coin, 44)[0]
                if length > 10000000 or stream.tell() + length + 5 > size:
                    raise ValueError('invalid coin length')
                stream.seek(length + 5, 1)
            length_bytes = stream.read(8)
            if len(length_bytes) != 8:
                raise ValueError('missing Orchard section')
            length = struct.unpack('<Q', length_bytes)[0]
            if length < 9 or stream.tell() + length + 32 != size:
                raise ValueError('invalid Orchard section length')
            section = stream.read(9)
            if section[:4] != b'ORCH' or struct.unpack_from('<I', section, 4)[0] != 1 or section[8] not in (1, 2):
                raise ValueError('invalid Orchard section profile')
            profile = 'full' if section[8] == 1 else 'compact'
            if profile == 'compact' and count:
                raise ValueError('compact snapshot carries coins')
        return version, height, header[8:40][::-1].hex(), profile


if __name__ == '__main__':
    import sys
    print(*inspect(sys.argv[1]))
