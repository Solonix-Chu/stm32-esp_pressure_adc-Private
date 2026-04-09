#!/usr/bin/env python3
"""Decode STM32 ADC logger captures into CSV and SVG waveform files."""

from __future__ import annotations

import argparse
import csv
import json
import struct
import sys
from array import array
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from xml.sax.saxutils import escape


FNV1A32_OFFSET_BASIS = 2166136261
FNV1A32_PRIME = 16777619

ADC_LINK_MAGIC = 0x314B4E4C
ADC_LINK_VERSION = 1
ADC_LINK_HEADER_BYTES = 40
ADC_LINK_PACKET_BYTES = 3112
ADC_LINK_PAYLOAD_BYTES = 3072
ADC_LINK_CHANNEL_COUNT = 6
ADC_LINK_SAMPLES_PER_CHANNEL = 256
ADC_LINK_BITS_PER_SAMPLE = 12
ADC_LINK_SAMPLE_RATE_HZ = 51470
ADC_LINK_FLAG_HALF = 0x0001
ADC_LINK_FLAG_FULL = 0x0002
ADC_REF_MV = 3300.0
ADC_MAX_RAW = 4095.0

HEADER_STRUCT = struct.Struct("<IHHIIIHHHHIII")

SVG_WIDTH = 1600
SVG_PANEL_HEIGHT = 180
SVG_LEFT_MARGIN = 96
SVG_RIGHT_MARGIN = 24
SVG_TOP_MARGIN = 48
SVG_BOTTOM_MARGIN = 48
SVG_MAX_BUCKETS = 1200

CHANNEL_COLORS = [
    "#166534",
    "#0f766e",
    "#1d4ed8",
    "#b45309",
    "#be123c",
    "#7c3aed",
]


@dataclass(frozen=True)
class ProtocolConfig:
    packet_bytes: int = ADC_LINK_PACKET_BYTES
    header_bytes: int = ADC_LINK_HEADER_BYTES
    payload_bytes: int = ADC_LINK_PAYLOAD_BYTES
    sample_rate_hz: int = ADC_LINK_SAMPLE_RATE_HZ
    channel_count: int = ADC_LINK_CHANNEL_COUNT
    samples_per_channel: int = ADC_LINK_SAMPLES_PER_CHANNEL
    bits_per_sample: int = ADC_LINK_BITS_PER_SAMPLE


@dataclass(frozen=True)
class PacketHeader:
    magic: int
    version: int
    header_bytes: int
    sequence: int
    tick_ms: int
    sample_rate_hz: int
    channel_count: int
    samples_per_channel: int
    bits_per_sample: int
    flags: int
    dropped_packets: int
    payload_bytes: int
    checksum: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Decode STM32 ADC logger .bin captures into CSV and SVG waveform files.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("input_bin", type=Path, help="Path to the recorded .bin file")
    parser.add_argument(
        "--metadata",
        type=Path,
        help="Path to sidecar .json metadata file; defaults to the same stem as the .bin file",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="Directory for decoded outputs; defaults to '<bin stem>_decoded' next to the input file",
    )
    parser.add_argument("--no-csv", action="store_true", help="Skip CSV export")
    parser.add_argument("--no-svg", action="store_true", help="Skip SVG waveform export")
    parser.add_argument(
        "--with-millivolts",
        action="store_true",
        help="Append millivolt columns to the CSV and use mV scale labels in the waveform",
    )
    parser.add_argument(
        "--max-packets",
        type=int,
        default=0,
        help="Decode only the first N packets; 0 means decode the whole file",
    )
    return parser.parse_args()


def load_metadata(path: Path | None) -> dict:
    if path is None or not path.exists():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def load_protocol_config(metadata: dict) -> ProtocolConfig:
    return ProtocolConfig(
        packet_bytes=int(metadata.get("packet_bytes", ADC_LINK_PACKET_BYTES)),
        header_bytes=int(metadata.get("header_bytes", ADC_LINK_HEADER_BYTES)),
        payload_bytes=int(metadata.get("payload_bytes", ADC_LINK_PAYLOAD_BYTES)),
        sample_rate_hz=int(metadata.get("sample_rate_hz", ADC_LINK_SAMPLE_RATE_HZ)),
        channel_count=int(metadata.get("channel_count", ADC_LINK_CHANNEL_COUNT)),
        samples_per_channel=int(metadata.get("samples_per_channel", ADC_LINK_SAMPLES_PER_CHANNEL)),
        bits_per_sample=int(metadata.get("bits_per_sample", ADC_LINK_BITS_PER_SAMPLE)),
    )


def fnv1a32(packet: bytes) -> int:
    checksum_offset = HEADER_STRUCT.size - 4
    value = FNV1A32_OFFSET_BASIS
    for index, byte in enumerate(packet):
        if checksum_offset <= index < checksum_offset + 4:
            byte = 0
        value ^= byte
        value = (value * FNV1A32_PRIME) & 0xFFFFFFFF
    return value


def parse_header(packet: bytes) -> PacketHeader:
    return PacketHeader(*HEADER_STRUCT.unpack_from(packet, 0))


def validate_header(header: PacketHeader, config: ProtocolConfig, packet: bytes) -> list[str]:
    errors: list[str] = []
    if header.magic != ADC_LINK_MAGIC:
        errors.append(f"magic=0x{header.magic:08x}")
    if header.version != ADC_LINK_VERSION:
        errors.append(f"version={header.version}")
    if header.header_bytes != config.header_bytes:
        errors.append(f"header_bytes={header.header_bytes}")
    if header.sample_rate_hz != config.sample_rate_hz:
        errors.append(f"sample_rate_hz={header.sample_rate_hz}")
    if header.channel_count != config.channel_count:
        errors.append(f"channel_count={header.channel_count}")
    if header.samples_per_channel != config.samples_per_channel:
        errors.append(f"samples_per_channel={header.samples_per_channel}")
    if header.bits_per_sample != config.bits_per_sample:
        errors.append(f"bits_per_sample={header.bits_per_sample}")
    if header.payload_bytes != config.payload_bytes:
        errors.append(f"payload_bytes={header.payload_bytes}")
    if header.flags & ~(ADC_LINK_FLAG_HALF | ADC_LINK_FLAG_FULL):
        errors.append(f"flags=0x{header.flags:04x}")
    calculated = fnv1a32(packet)
    if calculated != header.checksum:
        errors.append(f"checksum=0x{header.checksum:08x}/0x{calculated:08x}")
    return errors


def payload_to_words(packet: bytes, header: PacketHeader) -> array:
    words = array("H")
    payload = packet[header.header_bytes : header.header_bytes + header.payload_bytes]
    words.frombytes(payload)
    if sys.byteorder != "little":
        words.byteswap()
    return words


def flag_text(flags: int) -> str:
    parts: list[str] = []
    if flags & ADC_LINK_FLAG_HALF:
        parts.append("half")
    if flags & ADC_LINK_FLAG_FULL:
        parts.append("full")
    return "|".join(parts) if parts else f"0x{flags:04x}"


def raw_to_mv(raw: int) -> float:
    return (float(raw) * ADC_REF_MV) / ADC_MAX_RAW


def ensure_output_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def format_summary_lines(summary: dict) -> list[str]:
    lines = [
        f"input_bin: {summary['input_bin']}",
        f"metadata: {summary['metadata']}",
        f"output_dir: {summary['output_dir']}",
        f"packet_count: {summary['packet_count']}",
        f"valid_packets: {summary['valid_packets']}",
        f"invalid_packets: {summary['invalid_packets']}",
        f"trailing_bytes: {summary['trailing_bytes']}",
        f"frame_rows: {summary['frame_rows']}",
        f"duration_s: {summary['duration_s']:.6f}",
        f"sequence_gap_events: {summary['sequence_gap_events']}",
        f"missing_packets_by_sequence: {summary['missing_packets_by_sequence']}",
        f"stm32_drop_first: {summary['stm32_drop_first']}",
        f"stm32_drop_last: {summary['stm32_drop_last']}",
        f"stm32_drop_delta: {summary['stm32_drop_delta']}",
        f"flags: {summary['flags']}",
        f"csv_file: {summary['csv_file']}",
        f"svg_file: {summary['svg_file']}",
    ]
    if summary["invalid_examples"]:
        lines.append(f"invalid_examples: {summary['invalid_examples']}")
    return lines


def build_svg(
    channel_data: list[array],
    sample_rate_hz: int,
    output_path: Path,
    title: str,
    with_millivolts: bool,
) -> None:
    channel_count = len(channel_data)
    if channel_count == 0 or len(channel_data[0]) == 0:
        return

    samples_per_channel = len(channel_data[0])
    plot_width = SVG_WIDTH - SVG_LEFT_MARGIN - SVG_RIGHT_MARGIN
    panel_count = channel_count
    svg_height = SVG_TOP_MARGIN + (panel_count * SVG_PANEL_HEIGHT) + SVG_BOTTOM_MARGIN
    value_max = ADC_REF_MV if with_millivolts else ADC_MAX_RAW
    duration_s = (samples_per_channel - 1) / sample_rate_hz if samples_per_channel > 1 else 0.0

    def sample_value(raw: int) -> float:
        return raw_to_mv(raw) if with_millivolts else float(raw)

    def map_y(panel_index: int, value: float) -> float:
        panel_top = SVG_TOP_MARGIN + panel_index * SVG_PANEL_HEIGHT
        usable_height = SVG_PANEL_HEIGHT - 32
        clamped = min(max(value, 0.0), value_max)
        ratio = 1.0 - (clamped / value_max if value_max else 0.0)
        return panel_top + 16 + ratio * usable_height

    lines: list[str] = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{SVG_WIDTH}" height="{svg_height}" '
        f'viewBox="0 0 {SVG_WIDTH} {svg_height}">',
        '<rect width="100%" height="100%" fill="#f8fafc"/>',
        f'<text x="{SVG_LEFT_MARGIN}" y="28" font-size="20" font-family="monospace" fill="#111827">'
        f'{escape(title)}</text>',
    ]

    for tick_index in range(6):
        x = SVG_LEFT_MARGIN + (plot_width * tick_index / 5.0)
        label_time = duration_s * tick_index / 5.0
        lines.append(
            f'<line x1="{x:.2f}" y1="{SVG_TOP_MARGIN}" x2="{x:.2f}" y2="{svg_height - SVG_BOTTOM_MARGIN}" '
            'stroke="#e5e7eb" stroke-width="1"/>'
        )
        lines.append(
            f'<text x="{x:.2f}" y="{svg_height - 16}" font-size="11" text-anchor="middle" '
            'font-family="monospace" fill="#374151">'
            f"{label_time:.3f}s</text>"
        )

    for channel_index, samples in enumerate(channel_data):
        panel_top = SVG_TOP_MARGIN + channel_index * SVG_PANEL_HEIGHT
        panel_bottom = panel_top + SVG_PANEL_HEIGHT
        panel_label = f"CH{channel_index + 1}"
        lines.append(
            f'<rect x="{SVG_LEFT_MARGIN}" y="{panel_top}" width="{plot_width}" height="{SVG_PANEL_HEIGHT}" '
            'fill="#ffffff" stroke="#cbd5e1" stroke-width="1"/>'
        )
        lines.append(
            f'<text x="16" y="{panel_top + 26}" font-size="14" font-family="monospace" fill="#0f172a">'
            f'{panel_label}</text>'
        )
        lines.append(
            f'<text x="16" y="{panel_top + 44}" font-size="11" font-family="monospace" fill="#475569">'
            f"{'mV' if with_millivolts else 'raw'}</text>"
        )

        for ratio in (0.0, 0.25, 0.5, 0.75, 1.0):
            value = value_max * (1.0 - ratio)
            y = map_y(channel_index, value)
            lines.append(
                f'<line x1="{SVG_LEFT_MARGIN}" y1="{y:.2f}" x2="{SVG_WIDTH - SVG_RIGHT_MARGIN}" y2="{y:.2f}" '
                'stroke="#e2e8f0" stroke-width="1"/>'
            )
            lines.append(
                f'<text x="{SVG_LEFT_MARGIN - 8}" y="{y + 4:.2f}" font-size="10" text-anchor="end" '
                'font-family="monospace" fill="#64748b">'
                f"{value:.0f}</text>"
            )

        bucket_count = min(plot_width, SVG_MAX_BUCKETS, len(samples))
        if bucket_count <= 1:
            value = sample_value(samples[0])
            y = map_y(channel_index, value)
            x = SVG_LEFT_MARGIN + (plot_width / 2.0)
            lines.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="1.5" fill="{CHANNEL_COLORS[channel_index % len(CHANNEL_COLORS)]}"/>')
            continue

        for bucket_index in range(bucket_count):
            start = bucket_index * len(samples) // bucket_count
            end = max(start + 1, (bucket_index + 1) * len(samples) // bucket_count)
            bucket = samples[start:end]
            bucket_min = sample_value(min(bucket))
            bucket_max = sample_value(max(bucket))
            x = SVG_LEFT_MARGIN + (plot_width * bucket_index / max(bucket_count - 1, 1))
            y1 = map_y(channel_index, bucket_max)
            y2 = map_y(channel_index, bucket_min)
            lines.append(
                f'<line x1="{x:.2f}" y1="{y1:.2f}" x2="{x:.2f}" y2="{y2:.2f}" '
                f'stroke="{CHANNEL_COLORS[channel_index % len(CHANNEL_COLORS)]}" stroke-width="1"/>'
            )

        lines.append(
            f'<line x1="{SVG_LEFT_MARGIN}" y1="{panel_bottom}" x2="{SVG_WIDTH - SVG_RIGHT_MARGIN}" y2="{panel_bottom}" '
            'stroke="#94a3b8" stroke-width="1"/>'
        )

    lines.append("</svg>")
    output_path.write_text("\n".join(lines), encoding="utf-8")


def decode_capture(
    input_bin: Path,
    metadata_path: Path | None,
    output_dir: Path,
    write_csv: bool,
    write_svg: bool,
    with_millivolts: bool,
    max_packets: int,
) -> dict:
    metadata = load_metadata(metadata_path)
    config = load_protocol_config(metadata)
    raw = input_bin.read_bytes()

    packet_count_total, trailing_bytes = divmod(len(raw), config.packet_bytes)
    packet_limit = packet_count_total if max_packets <= 0 else min(max_packets, packet_count_total)

    channel_data = [array("H") for _ in range(config.channel_count)]
    csv_path = output_dir / f"{input_bin.stem}_samples.csv"
    svg_path = output_dir / f"{input_bin.stem}_waveform.svg"
    summary_path = output_dir / f"{input_bin.stem}_summary.json"
    csv_file = None
    csv_writer = None
    frame_rows = 0
    valid_packets = 0
    invalid_packets = 0
    invalid_examples: list[dict] = []
    sequence_gap_events = 0
    missing_packets_by_sequence = 0
    previous_sequence: int | None = None
    stm32_drop_first: int | None = None
    stm32_drop_last: int | None = None
    flag_counter: Counter[str] = Counter()

    if write_csv:
        csv_file = csv_path.open("w", newline="", encoding="utf-8")
        header_row = [
            "sample_index",
            "time_s",
            "packet_index",
            "packet_sequence",
            "sequence_gap_before",
            "packet_tick_ms",
            "packet_flags",
            "stm32_dropped_packets",
        ] + [f"ch{index + 1}_raw" for index in range(config.channel_count)]
        if with_millivolts:
            header_row.extend(f"ch{index + 1}_mv" for index in range(config.channel_count))
        csv_writer = csv.writer(csv_file)
        csv_writer.writerow(header_row)

    try:
        for packet_index in range(packet_limit):
            start = packet_index * config.packet_bytes
            packet = raw[start : start + config.packet_bytes]
            header = parse_header(packet)
            errors = validate_header(header, config, packet)
            if errors:
                invalid_packets += 1
                if len(invalid_examples) < 8:
                    invalid_examples.append(
                        {
                            "packet_index": packet_index,
                            "sequence": header.sequence,
                            "errors": errors,
                        }
                    )
                continue

            valid_packets += 1
            sequence_gap_before = 0
            if previous_sequence is not None and header.sequence != previous_sequence + 1:
                sequence_gap_events += 1
                if header.sequence > previous_sequence + 1:
                    missing_packets_by_sequence += header.sequence - previous_sequence - 1
                    sequence_gap_before = header.sequence - previous_sequence - 1
            previous_sequence = header.sequence

            if stm32_drop_first is None:
                stm32_drop_first = header.dropped_packets
            stm32_drop_last = header.dropped_packets
            flag_counter[flag_text(header.flags)] += 1

            payload_words = payload_to_words(packet, header)

            for frame_index in range(header.samples_per_channel):
                base = frame_index * header.channel_count
                row_raw = [payload_words[base + channel_index] for channel_index in range(header.channel_count)]
                for channel_index, sample in enumerate(row_raw):
                    channel_data[channel_index].append(sample)

                if csv_writer is None:
                    continue

                sample_index = frame_rows
                row = [
                    sample_index,
                    f"{sample_index / config.sample_rate_hz:.9f}",
                    packet_index,
                    header.sequence,
                    sequence_gap_before,
                    header.tick_ms,
                    flag_text(header.flags),
                    header.dropped_packets,
                    *row_raw,
                ]
                if with_millivolts:
                    row.extend(f"{raw_to_mv(value):.6f}" for value in row_raw)
                csv_writer.writerow(row)
                frame_rows += 1

            if csv_writer is None:
                frame_rows += header.samples_per_channel
    finally:
        if csv_file is not None:
            csv_file.close()

    if write_svg:
        build_svg(
            channel_data=channel_data,
            sample_rate_hz=config.sample_rate_hz,
            output_path=svg_path,
            title=f"{input_bin.name} ({valid_packets} packets, {frame_rows} frames)",
            with_millivolts=with_millivolts,
        )

    summary = {
        "input_bin": str(input_bin),
        "metadata": str(metadata_path) if metadata_path is not None else "",
        "output_dir": str(output_dir),
        "packet_count": packet_limit,
        "valid_packets": valid_packets,
        "invalid_packets": invalid_packets,
        "trailing_bytes": trailing_bytes,
        "frame_rows": frame_rows,
        "duration_s": frame_rows / config.sample_rate_hz if config.sample_rate_hz else 0.0,
        "sequence_gap_events": sequence_gap_events,
        "missing_packets_by_sequence": missing_packets_by_sequence,
        "stm32_drop_first": 0 if stm32_drop_first is None else stm32_drop_first,
        "stm32_drop_last": 0 if stm32_drop_last is None else stm32_drop_last,
        "stm32_drop_delta": 0
        if stm32_drop_first is None or stm32_drop_last is None
        else stm32_drop_last - stm32_drop_first,
        "flags": dict(flag_counter),
        "csv_file": str(csv_path) if write_csv else "",
        "svg_file": str(svg_path) if write_svg else "",
        "invalid_examples": invalid_examples,
    }
    summary_path.write_text(json.dumps(summary, indent=2, ensure_ascii=True), encoding="utf-8")
    return summary


def main() -> int:
    args = parse_args()
    input_bin = args.input_bin.resolve()
    if not input_bin.exists():
        print(f"input file not found: {input_bin}", file=sys.stderr)
        return 1

    metadata_path = args.metadata
    if metadata_path is None:
        candidate = input_bin.with_suffix(".json")
        metadata_path = candidate if candidate.exists() else None
    elif not metadata_path.exists():
        print(f"metadata file not found: {metadata_path}", file=sys.stderr)
        return 1

    output_dir = args.output_dir
    if output_dir is None:
        output_dir = input_bin.parent / f"{input_bin.stem}_decoded"
    output_dir = output_dir.resolve()
    ensure_output_dir(output_dir)

    if args.no_csv and args.no_svg:
        print("nothing to do: both CSV and SVG outputs are disabled", file=sys.stderr)
        return 1

    summary = decode_capture(
        input_bin=input_bin,
        metadata_path=metadata_path.resolve() if metadata_path is not None else None,
        output_dir=output_dir,
        write_csv=not args.no_csv,
        write_svg=not args.no_svg,
        with_millivolts=args.with_millivolts,
        max_packets=args.max_packets,
    )

    print("\n".join(format_summary_lines(summary)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
