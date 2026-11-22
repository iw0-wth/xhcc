"""Report compiler storage, declared Wasm memory and external package resources.

No third-party modules. These measurements do not estimate runtime RSS or MCU RAM.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
PAGE_BYTES = 65536


def uleb(data, offset):
    value = 0
    for shift in range(0, 70, 7):
        if offset >= len(data):
            raise ValueError('truncated LEB128')
        byte = data[offset]; offset += 1
        value |= (byte & 127) << shift
        if not byte & 128:
            return value, offset
    raise ValueError('oversized LEB128')


def wasm_memory(path):
    data = path.read_bytes()
    if data[:8] != b'\0asm\1\0\0\0':
        raise ValueError('expected Wasm version 1: ' + str(path))
    result = {'path': str(path), 'file_bytes': len(data),
              'sha256': hashlib.sha256(data).hexdigest(), 'memories': []}
    offset = 8
    while offset < len(data):
        kind = data[offset]; size, start = uleb(data, offset + 1)
        end = start + size
        if end > len(data):
            raise ValueError('truncated Wasm section')
        if kind == 5:
            count, pos = uleb(data, start)
            for _ in range(count):
                flags, pos = uleb(data, pos)
                minimum, pos = uleb(data, pos)
                maximum = None
                if flags & 1:
                    maximum, pos = uleb(data, pos)
                if flags & ~7:
                    raise ValueError('unsupported Wasm memory flags')
                result['memories'].append({'initial_pages': minimum, 'maximum_pages': maximum,
                    'initial_bytes': minimum * PAGE_BYTES,
                    'maximum_bytes': maximum * PAGE_BYTES if maximum is not None else None,
                    'shared': bool(flags & 2), 'memory64': bool(flags & 4)})
        offset = end
    if not result['memories']:
        result['memory_note'] = 'No defined memory section; memory may be imported. Not inferred.'
    return result


def parse_map(text):
    """Read wasm-ld Addr/Off/Size maps and common linker symbol assignments.

    __stack_pointer rows in wasm-ld's GLOBAL section contain a global index,
    not a linear address, and are deliberately excluded from boundaries.
    """
    boundaries = {}
    static_end = 0
    cache_sections = []
    names = {'__data_end', '__heap_base', '__heap_end', '__stack_low', '__stack_high', '__global_base'}
    for line in text.splitlines():
        assignment = re.search(r'\b(0x[0-9a-fA-F]+)\s+(__(?:data_end|heap_base|heap_end|stack_low|stack_high|global_base))\s*(?:=|$)', line)
        if assignment:
            boundaries[assignment[2]] = int(assignment[1], 16)
        row = re.match(r'^\s*([0-9a-fA-F]+|-)\s+([0-9a-fA-F]+|-)\s+([0-9a-fA-F]+)\s+(.*?)\s*$', line)
        if not row or row[1] == '-':
            continue
        address, size, tail = int(row[1], 16), int(row[3], 16), row[4]
        if tail in names:
            boundaries[tail] = address
        # Only allocated data input/output sections contribute static extent.
        if re.search(r'(?:^|:)\(?\.(?:bss|data|rodata|ark_buf)(?:[.)\s]|$)', tail):
            static_end = max(static_end, address + size)
            if ':(' in tail and any(word in tail.lower() for word in ('rules', 'template', 'cpu', 'encoding', 'selector', 'package_options')):
                cache_sections.append({'address': address, 'bytes': size, 'section': tail})
    result = {'symbols': boundaries, 'static_data_end_from_sections': static_end or None,
              'resource_related_sections': cache_sections,
              'resource_related_section_bytes': sum(item['bytes'] for item in cache_sections)}
    if '__stack_low' in boundaries and '__stack_high' in boundaries:
        result['stack_reserved_bytes'] = boundaries['__stack_high'] - boundaries['__stack_low']
    result['missing_boundary_symbols'] = sorted(names - boundaries.keys())
    return result


def package_resources(root):
    result = []
    for manifest in sorted(root.glob('*/package.ini')):
        directory = manifest.parent
        metadata = {}
        for line in manifest.read_text(encoding='utf-8').splitlines():
            if '=' in line and not line.lstrip().startswith(('#', ';')):
                key, value = line.split('=', 1); metadata[key.strip()] = value.strip()
        files = []
        for path in sorted(directory.rglob('*')):
            if not path.is_file():
                continue
            relative = path.relative_to(directory).as_posix()
            item = {'path': relative, 'bytes': path.stat().st_size}
            if path.suffix in ('.rules', '.ini'):
                text = path.read_text(encoding='utf-8')
                item['rule_records'] = len(re.findall(r'^\s*rule\s+', text, re.M))
                item['ini_sections'] = len(re.findall(r'^\s*\[[^\]\r\n]+\]\s*$', text, re.M))
                item['noncomment_lines'] = sum(bool(line.strip()) and not line.lstrip().startswith(('#', ';')) for line in text.splitlines())
            files.append(item)
        result.append({'id': metadata.get('id', directory.name), 'target': metadata.get('target'),
                       'total_bytes': sum(item['bytes'] for item in files), 'file_count': len(files),
                       'rules_bytes': sum(item['bytes'] for item in files if item['path'].endswith('.rules')),
                       'rule_records': sum(item.get('rule_records', 0) for item in files),
                       'files': files})
    return result


def markdown(report):
    module = report['wasm']
    lines = ['# xhcc resource budget', '', f"Compiler file: `{module['path']}`; {module['file_bytes']:,} bytes.", '',
             '| Memory | Initial bytes | Maximum bytes |', '|---|---:|---:|']
    for index, memory in enumerate(module['memories']):
        lines.append(f"| {index} | {memory['initial_bytes']:,} | {memory['maximum_bytes'] if memory['maximum_bytes'] is not None else 'unbounded'} |")
    lines += ['', '## Link map', '', f"Static data extent from allocated sections: {report['link_map'].get('static_data_end_from_sections')} bytes.",
              'Map addresses describe linear memory; code offsets and global indices are excluded.', '', '| Boundary symbol | Address |', '|---|---:|']
    for name, address in sorted(report['link_map'].get('symbols', {}).items()):
        lines.append(f'| `{name}` | {address} |')
    missing = report['link_map'].get('missing_boundary_symbols', [])
    lines += ['', 'Missing boundary symbols: ' + (', '.join(missing) or 'none') + '.', '',
              'Missing addresses remain unknown; stack size and heap headroom are not guessed.', '',
              '## External packages', '', '| Package | Files | Total bytes | Rules bytes | Rule records |', '|---|---:|---:|---:|---:|']
    for package in report['packages']:
        lines.append(f"| {package['id']} | {package['file_count']} | {package['total_bytes']:,} | {package['rules_bytes']:,} | {package['rule_records']} |")
    lines += ['', '## Measurement limits', '', *[f'- {note}' for note in report['limitations']], '']
    return '\n'.join(lines)


def self_test():
    sample = '''    Addr      Off     Size Out In Symbol
       0        0        0         __stack_pointer
     400      100       20 .data
     420        0       80 .bss
     420        0       10         cpu.o:(.bss.cpu_storage)
     4a0        0        0         __data_end
     4a0        0        0         __stack_low
     8a0        0        0         __stack_high
     8a0        0        0         __heap_base
       -      999      888 CODE
'''
    result = parse_map(sample)
    assert result['static_data_end_from_sections'] == 0x4a0
    assert result['stack_reserved_bytes'] == 1024
    assert result['symbols']['__heap_base'] == 0x8a0
    assert '__stack_pointer' not in result['symbols']
    assert result['resource_related_section_bytes'] == 16


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wasm', type=Path, default=ROOT / 'build/wasm/xhcc.wasm')
    parser.add_argument('--map', type=Path, default=ROOT / 'build/wasm/dev/all/xhcc.map')
    parser.add_argument('--package-root', type=Path, default=ROOT / 'packages')
    parser.add_argument('--json', type=Path, help='write full machine-readable report')
    parser.add_argument('--markdown', type=Path, help='write Markdown summary')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
    module = wasm_memory(args.wasm)
    link_map = parse_map(args.map.read_text(encoding='utf-8')) if args.map.is_file() else {'missing': True}
    link_map['path'] = str(args.map)
    report = {'schema_version': 1, 'wasm': module, 'link_map': link_map,
              'packages': package_resources(args.package_root), 'limitations': [
        'Wasm file bytes measure storage/download size, not runtime RAM.',
        'Declared linear-memory limits exclude engine metadata, native/JIT code, WASI host buffers, task stacks and filesystem caches.',
        'External resource bytes are stored data. They are not all loaded simultaneously and do not equal parser cache RAM.',
        'Resource-related map sections show reserved static buffers in this linked build; they are not an additional allocation on top of static data extent.',
        'The map must come from the same build. Post-link optimization may change static layout; verify exported boundaries in the executed module before treating map addresses as exact.',
        'Actual arena peaks require representative compiler --memory-report runs. Actual total MCU RAM/RSS requires measurements on the intended runtime and board.'
    ]}
    output = json.dumps(report, indent=2, ensure_ascii=False)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True); args.json.write_text(output + '\n', encoding='utf-8')
    if args.markdown:
        args.markdown.parent.mkdir(parents=True, exist_ok=True); args.markdown.write_text(markdown(report), encoding='utf-8')
    if not args.json and not args.markdown:
        print(output)
    else:
        print(f"compiler={module['file_bytes']} bytes; packages={sum(p['total_bytes'] for p in report['packages'])} bytes; static_end={link_map.get('static_data_end_from_sections')}")


if __name__ == '__main__':
    main()
