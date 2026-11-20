"""Read the US cartridge's map links and check the selected dispatch entries."""
from collections import Counter
from struct import Struct, unpack_from

from common import RecompError
from rnc import decode_rnc1


HEADER = Struct(">II")
CURRENT_CALL = 0x269c0c
REMOTE_CALL = 0x269c28


def require(condition, reason):
    if not condition:
        raise RecompError(f"Turok 3 level events: {reason}")


def members(data, start, end):
    """Indexed sets contain a count and count + 1 offsets relative to the set."""
    require(0 <= start <= end <= len(data) and end - start >= 8, "truncated indexed set")
    count = int.from_bytes(data[start:start + 4], "big")
    header_size = (count + 2) * 4
    require(header_size <= end - start, f"indexed set header exceeds span at {start:#x}")
    offsets = unpack_from(f">{count + 1}I", data, start + 4)
    require(header_size <= offsets[0] and offsets[-1] == end - start,
            f"incorrect indexed set extent at {start:#x}")
    require(all(a <= b for a, b in zip(offsets, offsets[1:])), f"unordered offsets at {start:#x}")
    return [(start + a, start + b) for a, b in zip(offsets, offsets[1:])]


def records(packet, span, width):
    start, end = span
    require(end - start >= HEADER.size, "truncated record set")
    stride, count = HEADER.unpack_from(packet, start)
    require(stride == width or (stride == 0 and count == 0), f"invalid {width}-byte record stride")
    start += HEADER.size
    size = count * width
    require((size + 7) & ~7 == end - start, f"incorrect {width}-byte record set extent")
    return [packet[offset:offset + width] for offset in range(start, start + size, width)]


def handler_catalog(rom, profile, coverage):
    spec = profile["level_events"]
    resident = next(region for region in profile["discovery"]["regions"] if region["name"] == ".resident")

    def word(address):
        offset = address - resident["vram"]
        require(0 <= offset <= resident["size"] - 4, f"unmapped bank address {address:#x}")
        return unpack_from(">I", rom, resident["rom_offset"] + offset)[0]

    banks = [word(spec["bank_directory"] + 4 * i) for i in range(spec["bank_count"])]
    sizes = {spec["empty_bank"]: 1}
    checked = set()
    for table in profile["discovery"]["function_tables"]:
        if table["pc"] not in (CURRENT_CALL, REMOTE_CALL):
            continue
        bank = table["table_vram"]
        field = 0 if table["pc"] == CURRENT_CALL else 4
        require(bank in banks and table["stride"] == 8 and table["field_offset"] == field,
                f"unexpected event table layout at {bank:#x}")
        require(sizes.setdefault(bank, table["count"]) == table["count"], f"inconsistent bank size at {bank:#x}")
        checked.update((bank, index, field) for index in table["indices"])
    translated = {f["vram"] for f in coverage["functions"] if f["status"] in ("ready", "hooked")}

    def lookup(kind, action):
        require(kind < len(banks), f"event type {kind} exceeds bank directory")
        bank = banks[kind]
        require(action < sizes.get(bank, 0), f"event {kind}/{action} exceeds checked bank at {bank:#x}")
        handlers = []
        for field in (0, 4):
            target = word(bank + 8 * action + field)
            if target:
                require((bank, action, field) in checked and target in translated,
                        f"unchecked handler {target:#x} for event {kind}/{action} at bank offset +{field}")
            handlers.append(target or None)
        return dict(bank_vram=bank, current_map_handler=handlers[0], remote_map_handler=handlers[1])

    return lookup


def read_level_events(rom, profile, coverage):
    spec = profile["level_events"]
    cart = HEADER.unpack_from(rom, spec["cart_segment_entry"])
    root = members(rom, *cart)
    require(spec["level_directory_slot"] < len(root), "missing level directory")
    levels = members(rom, *root[spec["level_directory_slot"]])
    lookup = handler_catalog(rom, profile, coverage)
    selectors, counts, maps = {}, Counter(), []
    for map_id, level in enumerate(levels):
        chunks = members(rom, *level)
        require(max(spec["link_slot"], spec["name_slot"]) < len(chunks), f"missing data for map {map_id}")
        name_bytes = rom[slice(*chunks[spec["name_slot"]])]
        name = name_bytes.partition(b"\0")[0].decode("ascii")
        start, end = chunks[spec["link_slot"]]
        try:
            packet = decode_rnc1(rom[start:end])
            sets = members(packet, 0, len(packet))
            require(len(sets) == 4, "expected two link groups")
            groups = []
            for key_member in (0, 2):
                keys = records(packet, sets[key_member], 4)
                events = records(packet, sets[key_member + 1], 24)
                require(len(keys) == len(events), "mismatched link key and event counts")
                entries = []
                for key, event in zip(keys, events):
                    kind, action = event[21:23]
                    target_map = int.from_bytes(event[10:12], "big")
                    require(target_map < len(levels), f"invalid target map {target_map}")
                    pair = (kind, action)
                    if pair not in selectors:
                        selectors[pair] = lookup(kind, action)
                    counts[kind, action, target_map == map_id] += 1
                    entries.append(dict(key=int.from_bytes(key, "big"), type=kind, action=action,
                                        target_map=target_map))
                groups.append(dict(key_member=key_member, event_member=key_member + 1, records=entries))
        except RecompError as exc:
            raise RecompError(f"map {map_id} ({name}), link packet at {start:#x}: {exc}") from exc
        maps.append(dict(index=map_id, name=name, packet_rom_offset=start, packet_rom_end=end, groups=groups))
    return dict(rom_sha1=profile["game"]["rom_sha1"], map_count=len(maps), record_count=sum(counts.values()),
                current_map_call_pc=CURRENT_CALL, remote_map_call_pc=REMOTE_CALL,
                selectors=[dict(type=k, action=a, **selectors[k, a], current_map_records=counts[k, a, True],
                                remote_map_records=counts[k, a, False]) for k, a in sorted(selectors)],
                maps=maps)
