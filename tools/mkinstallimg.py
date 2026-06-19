#!/usr/bin/env python3

import argparse
import array
import math
import pathlib
import struct
import uuid
import zlib


SECTOR_SIZE = 512
GPT_ENTRY_COUNT = 128
GPT_ENTRY_SIZE = 128
GPT_ENTRY_ARRAY_SECTORS = (GPT_ENTRY_COUNT * GPT_ENTRY_SIZE) // SECTOR_SIZE
ESP_START_LBA = 2048
# Reduced ESP size to speed up build and lower memory usage. The original
# 256 MiB ESP is larger than needed for the boot files. 64 MiB is sufficient
# for the typical payload while keeping the FAT32 layout valid.
ESP_MIN_BYTES = 64 * 1024 * 1024
ESP_SLACK_BYTES = 32 * 1024 * 1024
ESP_ALIGN_BYTES = 32 * 1024 * 1024
ROOT_ALIGN_SECTORS = 2048
FAT32_EOC = 0x0FFFFFF8

GUID_EFI_SYSTEM = bytes((
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B,
))
GUID_TIRAMISU_FS = bytes((
    0x54, 0x4E, 0x55, 0x00, 0x13, 0x37, 0x42, 0x42,
    0x80, 0x86, 0x54, 0x49, 0x52, 0x41, 0x4D, 0x49,
))


def align_up(value: int, alignment: int) -> int:
    return ((value + alignment - 1) // alignment) * alignment


def short_name(name: str) -> bytes:
    upper = name.upper()
    if upper in (".", ".."):
        return upper.encode("ascii").ljust(11, b" ")
    parts = upper.split(".")
    if len(parts) > 2:
        raise ValueError(f"unsupported FAT name: {name}")
    stem = parts[0][:8]
    ext = parts[1][:3] if len(parts) == 2 else ""
    return stem.ljust(8, " ").encode("ascii") + ext.ljust(3, " ").encode("ascii")


def utf16_name(name: str) -> bytes:
    encoded = name.encode("utf-16le")
    return encoded[:72].ljust(72, b"\x00")


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


class Fat32Image:
    def __init__(self, size_bytes: int, label: str) -> None:
        if size_bytes < 32 * 1024 * 1024:
            raise ValueError("FAT32 ESP image must be at least 32 MiB")

        self.size_bytes = size_bytes
        self.cluster_bytes = 0
        self.image = bytearray(size_bytes)

        total_sectors = size_bytes // SECTOR_SIZE
        reserved = 32
        fat_count = 2
        sectors_per_cluster = None
        fat_sectors = 0
        clusters = 0
        for candidate in (1, 2, 4, 8, 16, 32, 64, 128):
            trial_clusters = total_sectors // candidate
            trial_fat_sectors = ((trial_clusters + 2) * 4 + SECTOR_SIZE - 1) // SECTOR_SIZE
            trial_clusters = (total_sectors - reserved - fat_count * trial_fat_sectors) // candidate
            trial_fat_sectors = ((trial_clusters + 2) * 4 + SECTOR_SIZE - 1) // SECTOR_SIZE
            if 65525 <= trial_clusters <= 0x0FFFFFF5:
                sectors_per_cluster = candidate
                fat_sectors = trial_fat_sectors
                clusters = trial_clusters
                break
        if sectors_per_cluster is None:
            raise ValueError("ESP image size cannot be formatted as FAT32")

        self.total_sectors = total_sectors
        self.sectors_per_cluster = sectors_per_cluster
        self.cluster_bytes = sectors_per_cluster * SECTOR_SIZE
        self.reserved = reserved
        self.fat_count = fat_count
        self.fat_sectors = fat_sectors
        self.data_start_sector = reserved + fat_count * fat_sectors
        self.root_cluster = 2
        self.fat = [0] * (clusters + 2)
        self.fat[0] = 0x0FFFFFF8
        self.fat[1] = 0x0FFFFFFF
        self.fat[self.root_cluster] = FAT32_EOC
        self.next_free_cluster = self.root_cluster + 1

        self._format_bpb(label)

    def _format_bpb(self, label: str) -> None:
        label11 = label.upper()[:11].ljust(11, " ").encode("ascii")
        bpb = self.image
        struct.pack_into(
            "<3s8sHBHBHHBHHHII IHHIHH12sBBB I11s8s",
            bpb,
            0,
            b"\xEB\x58\x90",
            b"TIRAMISU",
            SECTOR_SIZE,
            self.sectors_per_cluster,
            self.reserved,
            self.fat_count,
            0,
            0,
            0xF8,
            0,
            63,
            255,
            0,
            self.total_sectors,
            self.fat_sectors,
            0,
            0,
            self.root_cluster,
            1,
            6,
            b"\x00" * 12,
            0x80,
            0,
            0x29,
            0x544E5531,
            label11,
            b"FAT32   ",
        )
        bpb[510:512] = b"\x55\xAA"

        fsinfo = self.image[SECTOR_SIZE:SECTOR_SIZE * 2]
        struct.pack_into("<I", fsinfo, 0, 0x41615252)
        struct.pack_into("<I", fsinfo, 484, 0x61417272)
        struct.pack_into("<I", fsinfo, 488, len(self.fat) - 3)
        struct.pack_into("<I", fsinfo, 492, self.next_free_cluster)
        struct.pack_into("<I", fsinfo, 508, 0xAA550000)

        self.image[6 * SECTOR_SIZE:7 * SECTOR_SIZE] = self.image[:SECTOR_SIZE]
        self.image[7 * SECTOR_SIZE:8 * SECTOR_SIZE] = self.image[SECTOR_SIZE:2 * SECTOR_SIZE]

    def cluster_offset(self, cluster: int) -> int:
        sector = self.data_start_sector + (cluster - 2) * self.sectors_per_cluster
        return sector * SECTOR_SIZE

    def allocate_chain(self, byte_count: int) -> int:
        clusters_needed = max(1, math.ceil(byte_count / self.cluster_bytes))
        first = self.next_free_cluster
        last = first + clusters_needed
        if last > len(self.fat):
            raise ValueError("ESP image ran out of clusters")
        for cluster in range(first, last):
            self.fat[cluster] = cluster + 1 if cluster + 1 < last else FAT32_EOC
        self.next_free_cluster = last
        return first

    def write_chain(self, first_cluster: int, data: bytes) -> None:
        remaining = data
        cluster = first_cluster
        while True:
            chunk = remaining[:self.cluster_bytes]
            remaining = remaining[self.cluster_bytes:]
            offset = self.cluster_offset(cluster)
            self.image[offset:offset + len(chunk)] = chunk
            if len(chunk) < self.cluster_bytes:
                self.image[offset + len(chunk):offset + self.cluster_bytes] = b"\x00" * (self.cluster_bytes - len(chunk))
            next_cluster = self.fat[cluster]
            if next_cluster >= FAT32_EOC:
                break
            cluster = next_cluster
        if remaining:
            raise ValueError("FAT32 cluster chain was too small for payload")

    @staticmethod
    def make_entry(name: str, attr: int, cluster: int, size: int) -> bytes:
        entry = bytearray(32)
        entry[0:11] = short_name(name)
        entry[11] = attr
        struct.pack_into("<H", entry, 20, (cluster >> 16) & 0xFFFF)
        struct.pack_into("<H", entry, 26, cluster & 0xFFFF)
        struct.pack_into("<I", entry, 28, size)
        return bytes(entry)

    def write_directory(self, cluster: int, entries: list[bytes], parent_cluster: int | None) -> None:
        payload = bytearray()
        if parent_cluster is not None:
            payload += self.make_entry(".", 0x10, cluster, 0)
            payload += self.make_entry("..", 0x10, parent_cluster, 0)
        for entry in entries:
            payload += entry
        payload += b"\x00" * 32
        clusters_needed = max(1, math.ceil(len(payload) / self.cluster_bytes))
        if clusters_needed != 1:
            raise ValueError("directory payload exceeded the current fixed single-cluster layout")
        self.write_chain(cluster, payload)

    def finalize(self) -> bytes:
        fat_bytes = array.array("I", (v & 0x0FFFFFFF for v in self.fat)).tobytes()
        for fat_index in range(self.fat_count):
            offset = (self.reserved + fat_index * self.fat_sectors) * SECTOR_SIZE
            self.image[offset:offset + len(fat_bytes)] = fat_bytes
        struct.pack_into("<I", self.image, SECTOR_SIZE + 492, self.next_free_cluster)
        self.image[7 * SECTOR_SIZE:8 * SECTOR_SIZE] = self.image[SECTOR_SIZE:2 * SECTOR_SIZE]
        return bytes(self.image)


def build_esp_image(efi_boot: bytes, kernel: bytes, rootfs: bytes, grub_cfg: bytes, grub_font: bytes) -> bytes:
    # Exclude rootfs from payload calculation since it's not in the ESP
    payload_bytes = len(efi_boot) + len(kernel) + len(grub_cfg) + len(grub_font)
    esp_size = max(ESP_MIN_BYTES, align_up(payload_bytes + ESP_SLACK_BYTES, ESP_ALIGN_BYTES))
    fat = Fat32Image(esp_size, "TIRAMISU")

    # TEMPORARY: Rootfs excluded from ESP to speed up build (~200 MB in bytearray is slow).
    # GRUB will load root.tfs from /boot/root.tfs on the ISO or installed partition instead.
    files = {
        "BOOTX64.EFI": efi_boot,
        "KERNEL.ELF": kernel,
        "GRUB.CFG": grub_cfg,
        "UNICODE.PF2": grub_font,
    }
    file_clusters = {}
    for name, data in files.items():
        cluster = fat.allocate_chain(len(data))
        fat.write_chain(cluster, data)
        file_clusters[name] = cluster

    fonts_dir = fat.allocate_chain(fat.cluster_bytes)
    grub_dir = fat.allocate_chain(fat.cluster_bytes)
    boot_dir = fat.allocate_chain(fat.cluster_bytes)
    efi_boot_dir = fat.allocate_chain(fat.cluster_bytes)
    efi_dir = fat.allocate_chain(fat.cluster_bytes)

    fat.write_directory(
        fonts_dir,
        [fat.make_entry("UNICODE.PF2", 0x20, file_clusters["UNICODE.PF2"], len(grub_font))],
        grub_dir,
    )
    fat.write_directory(
        grub_dir,
        [
            fat.make_entry("GRUB.CFG", 0x20, file_clusters["GRUB.CFG"], len(grub_cfg)),
            fat.make_entry("FONTS", 0x10, fonts_dir, 0),
        ],
        boot_dir,
    )
    # The ESP contains only the kernel and GRUB directory (rootfs excluded for speed).
    fat.write_directory(
        boot_dir,
        [
            fat.make_entry("KERNEL.ELF", 0x20, file_clusters["KERNEL.ELF"], len(kernel)),
            fat.make_entry("GRUB", 0x10, grub_dir, 0),
        ],
        fat.root_cluster,
    )
    fat.write_directory(
        efi_boot_dir,
        [fat.make_entry("BOOTX64.EFI", 0x20, file_clusters["BOOTX64.EFI"], len(efi_boot))],
        efi_dir,
    )
    fat.write_directory(
        efi_dir,
        [fat.make_entry("BOOT", 0x10, efi_boot_dir, 0)],
        fat.root_cluster,
    )
    fat.write_directory(
        fat.root_cluster,
        [
            fat.make_entry("EFI", 0x10, efi_dir, 0),
            fat.make_entry("BOOT", 0x10, boot_dir, 0),
        ],
        None,
    )
    return fat.finalize()


def protective_mbr(total_sectors: int) -> bytes:
    mbr = bytearray(SECTOR_SIZE)
    mbr[446 + 4] = 0xEE
    struct.pack_into("<I", mbr, 446 + 8, 1)
    struct.pack_into("<I", mbr, 446 + 12, min(total_sectors - 1, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xAA"
    return bytes(mbr)


def build_partition_entries(esp_first: int, esp_last: int, root_first: int, root_last: int) -> bytes:
    entries = bytearray(GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)

    def set_entry(index: int, type_guid: bytes, partition_guid: bytes, first_lba: int, last_lba: int, name: str) -> None:
        offset = index * GPT_ENTRY_SIZE
        entries[offset:offset + 16] = type_guid
        entries[offset + 16:offset + 32] = partition_guid
        struct.pack_into("<QQQ", entries, offset + 32, first_lba, last_lba, 0)
        entries[offset + 56:offset + 128] = utf16_name(name)

    set_entry(0, GUID_EFI_SYSTEM, uuid.uuid4().bytes_le, esp_first, esp_last, "EFI System")
    set_entry(1, GUID_TIRAMISU_FS, uuid.uuid4().bytes_le, root_first, root_last, "Tiramisu Root")
    return bytes(entries)


def build_gpt_header(current_lba: int, backup_lba: int, first_usable: int, last_usable: int,
                     partition_entry_lba: int, entries: bytes, disk_guid: bytes) -> bytes:
    header = bytearray(SECTOR_SIZE)
    struct.pack_into("<8sIIIIQQQQ16sQIII", header, 0,
                     b"EFI PART", 0x00010000, 92, 0, 0,
                     current_lba, backup_lba, first_usable, last_usable,
                     disk_guid, partition_entry_lba, GPT_ENTRY_COUNT, GPT_ENTRY_SIZE, crc32(entries))
    struct.pack_into("<I", header, 16, 0)
    struct.pack_into("<I", header, 16, crc32(header[:92]))
    return bytes(header)


def write_install_image(out_file, efi_boot: bytes, kernel: bytes, rootfs: bytes, grub_cfg: bytes, grub_font: bytes) -> int:
    esp_image = build_esp_image(efi_boot, kernel, rootfs, grub_cfg, grub_font)
    esp_sectors = len(esp_image) // SECTOR_SIZE
    esp_first = ESP_START_LBA
    esp_last = esp_first + esp_sectors - 1

    root_first = align_up(esp_last + 1, ROOT_ALIGN_SECTORS)
    root_sectors = math.ceil(len(rootfs) / SECTOR_SIZE)
    root_last = root_first + root_sectors - 1

    total_sectors = root_last + GPT_ENTRY_ARRAY_SECTORS + 2
    backup_entries_lba = total_sectors - GPT_ENTRY_ARRAY_SECTORS - 1
    first_usable = 2 + GPT_ENTRY_ARRAY_SECTORS
    last_usable = backup_entries_lba - 1

    entries = build_partition_entries(esp_first, esp_last, root_first, root_last)
    disk_guid = uuid.uuid4().bytes_le
    primary_header = build_gpt_header(1, total_sectors - 1, first_usable, last_usable, 2, entries, disk_guid)
    backup_header = build_gpt_header(total_sectors - 1, 1, first_usable, last_usable,
                                     backup_entries_lba, entries, disk_guid)

    # Write directly to file instead of building huge bytearray in RAM
    out_file.seek(0)
    out_file.write(protective_mbr(total_sectors))
    out_file.seek(SECTOR_SIZE)
    out_file.write(primary_header)
    out_file.seek(2 * SECTOR_SIZE)
    out_file.write(entries)
    out_file.seek(esp_first * SECTOR_SIZE)
    out_file.write(esp_image)
    out_file.seek(root_first * SECTOR_SIZE)
    out_file.write(rootfs)
    out_file.seek(backup_entries_lba * SECTOR_SIZE)
    out_file.write(entries)
    out_file.seek((total_sectors - 1) * SECTOR_SIZE)
    out_file.write(backup_header)
    
    return total_sectors * SECTOR_SIZE


def read_bytes(path: str) -> bytes:
    return pathlib.Path(path).read_bytes()


def main() -> int:
    parser = argparse.ArgumentParser(description="Build the raw disk template used by sysinstall.")
    parser.add_argument("--out", required=True)
    parser.add_argument("--efi", required=True)
    parser.add_argument("--kernel", required=True)
    parser.add_argument("--rootfs", required=True)
    parser.add_argument("--grub-cfg", required=True)
    parser.add_argument("--grub-font", required=True)
    args = parser.parse_args()

    print(f"Loading input files...")
    efi_boot = pathlib.Path(args.efi).read_bytes()
    kernel = pathlib.Path(args.kernel).read_bytes()
    rootfs = pathlib.Path(args.rootfs).read_bytes()
    grub_cfg = pathlib.Path(args.grub_cfg).read_bytes()
    grub_font = pathlib.Path(args.grub_font).read_bytes()
    
    print(f"Building install image...")

    out_path = pathlib.Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    
    with open(out_path, "wb") as out_file:
        image_size = write_install_image(out_file, efi_boot, kernel, rootfs, grub_cfg, grub_font)
    
    print(f"wrote {out_path} ({image_size // 1024} KiB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
