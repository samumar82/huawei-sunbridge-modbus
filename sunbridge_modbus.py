#!/usr/bin/env python3
"""Huawei SunBridge Modbus.

LAN-only Modbus TCP cache/write-through proxy for Huawei SUN2000 installations.

Based on cloudapp-dev/sun2000-modbus-cache (MIT License), with changes derived
from real-world SDongleA-05 testing: connection settling delay, conservative
inter-request pacing, complete TCP frame reads, broader Huawei Solar discovery
register coverage, and serialized upstream access.
"""

import asyncio
import logging
import os
import signal
import struct
import time

SDONGLE_HOST = os.environ.get("SUN2000_HOST")
SDONGLE_PORT = int(os.environ.get("SUN2000_PORT", 502))
DEVICE_ID = int(os.environ.get("SUN2000_UNIT_ID", 1))
SERVER_HOST = os.environ.get("LISTEN_HOST", "0.0.0.0")
SERVER_PORT = int(os.environ.get("LISTEN_PORT", 5502))
POLL_INTERVAL = int(os.environ.get("POLL_INTERVAL", 10))
CONNECT_WAIT = float(os.environ.get("CONNECT_WAIT", 2.0))
BATCH_DELAY = float(os.environ.get("BATCH_DELAY", 0.5))

if not SDONGLE_HOST:
    raise SystemExit("SUN2000_HOST is not set")

REGISTER_BATCHES = [
    (30000, 15),
    (30015, 50),
    (30071, 1),
    (30075, 2),
    (32000, 20),
    (32064, 52),
    (37100, 38),
    (37200, 2),
    (37760, 28),
    (40126, 2),
    (42900, 10),
    (43006, 2),
    (47000, 1),
    (47089, 1),
]

# Huawei Solar performs identification/setup reads in this area before normal
# polling. Requests not already covered by the cache are forwarded upstream.
SETUP_PASSTHROUGH_START = 30000
SETUP_PASSTHROUGH_END = 30071

logging.basicConfig(
    level=os.environ.get("LOG_LEVEL", "INFO"),
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
logger = logging.getLogger("huawei_sunbridge")

register_cache = {}
cache_lock = asyncio.Lock()
upstream_lock = asyncio.Lock()
last_update = 0.0


def init_cache():
    for start, count in REGISTER_BATCHES:
        for i in range(count):
            register_cache[start + i] = 0


async def open_upstream():
    reader, writer = await asyncio.wait_for(
        asyncio.open_connection(SDONGLE_HOST, SDONGLE_PORT), timeout=10
    )
    # Critical on the tested SDongleA-05: immediate first requests were unreliable.
    await asyncio.sleep(CONNECT_WAIT)
    return reader, writer


async def close_writer(writer):
    try:
        writer.close()
        await writer.wait_closed()
    except Exception:
        pass


async def read_response(reader):
    """Read exactly one complete Modbus TCP ADU."""
    mbap = await asyncio.wait_for(reader.readexactly(7), timeout=5)
    tx_id, proto, length, unit_id = struct.unpack(">HHHB", mbap)
    if proto != 0 or length < 2:
        raise ValueError("Invalid Modbus TCP header")
    pdu = await asyncio.wait_for(reader.readexactly(length - 1), timeout=5)
    return tx_id, unit_id, pdu


async def read_batch(reader, writer, start, count):
    req = struct.pack(">HHHBBHH", 0, 0, 6, DEVICE_ID, 3, start, count)
    writer.write(req)
    await writer.drain()
    _, _, pdu = await read_response(reader)
    if not pdu or pdu[0] >= 0x80 or len(pdu) < 2:
        return None
    byte_count = pdu[1]
    data = pdu[2:2 + byte_count]
    if len(data) < count * 2:
        return None
    return struct.unpack(">" + "H" * count, data[:count * 2])


async def direct_read(unit_id, start, count):
    """Perform one serialized upstream FC3 read and return uint16 values."""
    async with upstream_lock:
        writer = None
        try:
            reader, writer = await open_upstream()
            req = struct.pack(">HHHBBHH", 0, 0, 6, unit_id, 3, start, count)
            writer.write(req)
            await writer.drain()
            _, _, pdu = await read_response(reader)
            if not pdu or pdu[0] >= 0x80 or len(pdu) < 2:
                return None
            byte_count = pdu[1]
            data = pdu[2:2 + byte_count]
            if len(data) < count * 2:
                return None
            return struct.unpack(">" + "H" * count, data[:count * 2])
        except Exception as exc:
            logger.warning("Direct read %s/%s failed: %s", start, count, exc)
            return None
        finally:
            if writer:
                await close_writer(writer)


async def read_sdongle():
    """Refresh all cached batches using one controlled upstream session."""
    async with upstream_lock:
        writer = None
        try:
            reader, writer = await open_upstream()
        except Exception as exc:
            logger.warning("SDongle connect failed: %s", exc)
            return False

        success_count = 0
        fail_count = 0
        try:
            for start, count in REGISTER_BATCHES:
                try:
                    values = await read_batch(reader, writer, start, count)
                    if values is not None:
                        async with cache_lock:
                            for i, value in enumerate(values):
                                register_cache[start + i] = value
                        success_count += 1
                    else:
                        fail_count += 1
                        logger.warning("Batch failed: start=%s count=%s", start, count)
                    await asyncio.sleep(BATCH_DELAY)
                except asyncio.TimeoutError:
                    fail_count += 1
                    logger.warning("Batch timeout: start=%s count=%s", start, count)
                except Exception as exc:
                    fail_count += 1
                    logger.warning("Batch %s/%s error: %s", start, count, exc)
                    break
        finally:
            await close_writer(writer)

        if success_count:
            logger.info("Poll: %s/%s batches OK", success_count, success_count + fail_count)
        return success_count > 0


async def reader_loop():
    global last_update
    retry_delay = 5
    while True:
        success = await read_sdongle()
        if success:
            last_update = time.time()
            retry_delay = POLL_INTERVAL
        else:
            retry_delay = min(retry_delay, 10)
            age = time.time() - last_update if last_update else -1
            if age > 120:
                logger.warning("Cache stale for %.0fs", age)
        await asyncio.sleep(retry_delay)


async def forward_write(unit_id, reg_addr, reg_value):
    async with upstream_lock:
        writer = None
        try:
            reader, writer = await open_upstream()
            req = struct.pack(">HHHBBHH", 0, 0, 6, unit_id, 6, reg_addr, reg_value)
            writer.write(req)
            await writer.drain()
            _, _, pdu = await read_response(reader)
            ok = len(pdu) >= 5 and pdu[0] == 6
            if ok:
                logger.info("Write forwarded OK: register %s = %s", reg_addr, reg_value)
            return ok
        except Exception as exc:
            logger.warning("Write forward failed: %s", exc)
            return False
        finally:
            if writer:
                await close_writer(writer)


def modbus_response(tx_id, unit_id, pdu):
    return struct.pack(">HHHB", tx_id, 0, len(pdu) + 1, unit_id) + pdu


async def handle_client(client_reader, client_writer):
    peer = client_writer.get_extra_info("peername")
    logger.info("Client connected: %s", peer)
    try:
        while True:
            header = await asyncio.wait_for(client_reader.readexactly(7), timeout=60)
            tx_id, proto, length, unit_id = struct.unpack(">HHHB", header)
            if proto != 0 or length < 2:
                break
            pdu = await asyncio.wait_for(client_reader.readexactly(length - 1), timeout=5)
            fc = pdu[0]

            if fc == 3 and len(pdu) >= 5:
                reg_addr, reg_count = struct.unpack(">HH", pdu[1:5])
                async with cache_lock:
                    cached = all((reg_addr + i) in register_cache for i in range(reg_count))
                    values = [register_cache[reg_addr + i] for i in range(reg_count)] if cached else None

                # Never silently invent zero for an unknown discovery register.
                if values is None and SETUP_PASSTHROUGH_START <= reg_addr <= SETUP_PASSTHROUGH_END:
                    values = await direct_read(unit_id, reg_addr, reg_count)

                if values is None:
                    resp_pdu = struct.pack(">BB", 0x83, 2)  # Illegal data address
                else:
                    resp_pdu = struct.pack(">BB", 3, reg_count * 2)
                    resp_pdu += struct.pack(">" + "H" * reg_count, *values)
                client_writer.write(modbus_response(tx_id, unit_id, resp_pdu))
                await client_writer.drain()

            elif fc == 6 and len(pdu) >= 5:
                reg_addr, reg_value = struct.unpack(">HH", pdu[1:5])
                ok = await forward_write(unit_id, reg_addr, reg_value)
                resp_pdu = struct.pack(">BHH", 6, reg_addr, reg_value) if ok else struct.pack(">BB", 0x86, 4)
                client_writer.write(modbus_response(tx_id, unit_id, resp_pdu))
                await client_writer.drain()
            else:
                client_writer.write(modbus_response(tx_id, unit_id, struct.pack(">BB", fc | 0x80, 1)))
                await client_writer.drain()

    except (asyncio.TimeoutError, asyncio.IncompleteReadError, ConnectionResetError):
        pass
    except Exception as exc:
        logger.warning("Client %s error: %s", peer, exc)
    finally:
        logger.info("Client disconnected: %s", peer)
        await close_writer(client_writer)


async def main():
    init_cache()
    reader_task = asyncio.create_task(reader_loop())
    server = await asyncio.start_server(handle_client, SERVER_HOST, SERVER_PORT)
    logger.info("Huawei SunBridge listening on %s:%s", SERVER_HOST, SERVER_PORT)
    logger.info("Upstream SDongle %s:%s, unit %s", SDONGLE_HOST, SDONGLE_PORT, DEVICE_ID)
    logger.info("Caching %s registers in %s batches", sum(c for _, c in REGISTER_BATCHES), len(REGISTER_BATCHES))

    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(sig, stop.set)

    await stop.wait()
    reader_task.cancel()
    server.close()
    await server.wait_closed()


if __name__ == "__main__":
    asyncio.run(main())
