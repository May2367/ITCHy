#!/usr/bin/env python3
import ipaddress
import json
import re
import sys

if len(sys.argv) != 2:
    raise SystemExit('usage: get_node_data_ip.py NODE_NAME')
node_name = sys.argv[1]
data = json.load(sys.stdin)
fabnet = ipaddress.ip_network('10.128.0.0/10')


def walk(obj):
    if isinstance(obj, dict):
        yield obj
        for value in obj.values():
            yield from walk(value)
    elif isinstance(obj, list):
        for item in obj:
            yield from walk(item)


def contains_node_name(obj):
    for key in ('name', 'node_name'):
        if obj.get(key) == node_name:
            return True
    return False


def collect_strings(obj):
    if isinstance(obj, str):
        yield obj
    elif isinstance(obj, dict):
        for value in obj.values():
            yield from collect_strings(value)
    elif isinstance(obj, list):
        for value in obj:
            yield from collect_strings(value)

candidates = []
for obj in walk(data):
    if isinstance(obj, dict) and contains_node_name(obj):
        for text in collect_strings(obj):
            for match in re.findall(r'(?:\d{1,3}\.){3}\d{1,3}', text):
                try:
                    ip = ipaddress.ip_address(match)
                except ValueError:
                    continue
                if ip in fabnet:
                    candidates.append(str(ip))

if not candidates:
    raise SystemExit(f'could not find FABNetv4 address for node {node_name}')

print(candidates[0])
