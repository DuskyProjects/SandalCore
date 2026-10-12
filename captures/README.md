# Captures

Community packet captures of the Ashes of Creation client talking to the game servers, anonymized, for protocol work.

**Credit:** captured by the **CoA (Creation of Ashes)** community and the Ashes community who took part in capturing the PCAPs.
`creation-of-ashes/` is a copy of their capture repository (see `creation-of-ashes/README.md`, their own credit note). Thank you.

## What is included

| Set | Files | Size (MB) | Details |
|---|---:|---:|---|
| `creation-of-ashes/` | 1106 | ~213 MB | 1,105 unique `.pcap` packet captures + community README |
| `hexdumps.zip` (separate download) | 1 | 152.2 MB | 996 `.txt` reconstructed packet hexdumps (compressed) |

The captures in this folder take about 213 MB; duplicate captures have been removed. The 996 text hexdumps are too big for the repository (GitHub limits files to 100 MB), so `hexdumps.zip` is a separate download on the [navmesh-v1 release](https://github.com/cyborqz/SandalCore/releases/tag/navmesh-v1). Download it and unzip it into this `captures/` folder if you want them; you only need them to read packets without Wireshark.

### `creation-of-ashes/` by top-level folder

| Folder | `.pcap` files | `.pcap` MB | `.txt` hexdumps (in `hexdumps.zip`, release download) |
|---|---:|---:|---:|
| `Classes/` | 395 | 55.3 | 429 |
| `Geode/` | 1 | 2.1 | 1 |
| `Ghost/` | 2 | 0.9 | 2 |
| `artisan/` | 90 | 14.5 | 78 |
| `character/` | 36 | 9.4 | 28 |
| `combat/` | 18 | 3.6 | 12 |
| `economy/` | 66 | 6.3 | 48 |
| `mounts/` | 46 | 10.4 | 27 |
| `nodes/` | 32 | 15.7 | 14 |
| `quests_commissions/` | 5 | 0.3 | 1 |
| `server_workers/` | 6 | 7.4 | 5 |
| `social/` | 33 | 3.7 | 36 |
| `transport_caravans/` | 46 | 18.8 | 43 |
| `transport_caravels/` | 5 | 2.0 | 10 |
| `transport_crates/` | 35 | 5.7 | 20 |
| `ui_menues_inventory_management/` | 169 | 23.5 | 94 |
| `weapon skill tree/` | 9 | 0.8 | 2 |
| `world_events/` | 11 | 1.7 | 0 |
| `world_gathering/` | 91 | 29.9 | 78 |
| `world_npc_interaction/` | 9 | 1.3 | 51 |

## How it is organized

* The folder layout of `creation-of-ashes/` is the community repository's own: topic folders (`Classes/<class>/`, `mounts/`, `nodes/`,
  `artisan/`, ...) with one scenario per file, often a `.pcap` plus a `*_hexdump` text next to it. File names are unchanged.
* `undefined_`: names (or folders) prefixed with `undefined_` mean "we do not know exactly what happens in this capture".
  A folder named `undefined/` means the same (35 files sit under such names).
* Some files with a `.pcap` extension in the original repository are not captures at all but text hexdumps. They are kept as text
  and got `.txt` appended to the name so that Wireshark does not try to open them (644 files).
* Duplicate captures (exact byte-for-byte `_1.pcap` sibling clones and cross-folder copies) were deduplicated.
* `.pcapng` originals are now `.pcap` with the same base name.

## The two file kinds

1. **`.pcap`**: classic libpcap, Ethernet link type, same timestamp resolution as the original. Open them with Wireshark
   (`File > Open`). The game uses UDP with a custom protocol on a server port in the 7000s (for example 7486), so Wireshark shows
   it as plain UDP data; the login/telemetry side uses TLS (TCP) and is encrypted.
2. **`.txt` hexdumps**: written by the community capture tool: a few header comment lines, then per packet `--- Packet N ---`, the
   `IP:` and `UDP:`/`TCP:` lines and the payload as hex rows. They carry the IP addresses, ports and the payload only (no MAC addresses).

## How the files were anonymized

Raw captures expose the person who captured them: the PC's hardware, OS and capture-tool strings and the interface names in pcapng files,
MAC addresses (ARP and Ethernet headers), the home LAN addresses, DNS lookups, other apps' traffic (browsers, Discord, Steam, QUIC).
For every capture (1344 of them were written):

* **Kept only the game flows.** Game UDP flows (recognised by the game protocol's magic number on the packets) and the TCP/TLS flows
  to Ashes of Creation / Intrepid / Epic Online Services hosts (decided from the TLS server name and DNS answers inside the capture).
  Everything else was dropped: DNS, mDNS, LLMNR, NBNS, SSDP, DHCP, ARP, IPv6 noise, plain HTTP, other TLS/QUIC, voice/chat apps.
  Packets: 1,917,649 in, 1,864,196 kept (97.2 %), measured over the 1344 written captures.
* **Rewrote headers only.** The capturer's IPv4 address(es) became `10.0.0.2` (other LAN hosts `10.0.0.3` and up), MAC addresses
  became locally administered placeholders (capturer `02:00:00:00:00:02`, gateway `02:00:00:00:00:01`). The public game-server
  addresses are kept (Intrepid infrastructure, not personal). IPv4, UDP and TCP checksums were recomputed.
* **Converted to classic libpcap.** This drops everything pcapng stores besides packets: machine description, OS, capture application,
  interface names, comments and decryption-secret blocks.
* **Hexdump text files:** the `Local IP` header value is `[anonymized]`, `IP:` lines use the same placeholder mapping, payload hex rows
  are copied unchanged. 77 text copies that were byte-identical to a sibling were not copied twice.

**What did NOT change:** every payload byte of the kept flows and every packet timestamp. Game packets can therefore contain in-game
data such as character names, ids and chat text. Payloads are never edited, so a few payloads contain byte patterns that look like
an address by coincidence (random-looking game data); they are not headers.

**Not included:** 1 capture without any game traffic (a browsing capture), 12 reviewed non-capture files from the source
repository (helper scripts, dry-run logs, compiled Python, placeholder notes), and the other apps' traffic described above.

## Checks that were run on the result

* For each of the 1344 `.pcap` files, the multiset of (timestamp, UDP/TCP payload hash) of the kept flows equals the original's: 1344 of 1344 match.
* No output header contains any original capturer IP or MAC address of its file (0 hits). No ARP, DNS, HTTP or non-game TLS remains
  (Wireshark protocol hierarchy: only Ethernet, IPv4, UDP, TCP, TLS and data). 0 checksum errors.
* Hexdump text files: no private address in dotted or colon form, no MAC in any form, and only the `IP:` and header lines differ from the source.

The anonymization and the checks were done with scripts that are not part of this repository.
