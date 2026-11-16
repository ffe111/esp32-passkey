import struct

import uhid_bridge as ub


def test_event_sizes_match_kernel_struct():
    ev = ub.create2_event("x", 0x1209, 0x0001, ub.FIDO_REPORT_DESCRIPTOR)
    assert len(ev) == ub.UHID_EVENT_SIZE
    assert struct.unpack_from("<I", ev)[0] == ub.UHID_CREATE2
    # rd_size, bus, vendor, product follow name/phys/uniq
    rd_size, bus, vid, pid = struct.unpack_from("<HHII", ev, 4 + 128 + 64 + 64)
    assert (rd_size, bus, vid, pid) == (len(ub.FIDO_REPORT_DESCRIPTOR), ub.BUS_USB, 0x1209, 0x0001)
    assert len(ub.input2_event(b"\0" * 64)) == ub.UHID_EVENT_SIZE


def test_parse_output_strips_report_number():
    report = bytes(range(64))
    for raw in (report, b"\0" + report):
        body = raw.ljust(ub.UHID_DATA_MAX, b"\0") + struct.pack("<HB", len(raw), 1)
        ev = struct.pack("<I", ub.UHID_OUTPUT) + body
        assert ub.parse_output(ev.ljust(ub.UHID_EVENT_SIZE, b"\0")) == report


def test_non_output_events_ignored():
    ev = struct.pack("<I", ub.UHID_OPEN).ljust(ub.UHID_EVENT_SIZE, b"\0")
    assert ub.parse_output(ev) is None
