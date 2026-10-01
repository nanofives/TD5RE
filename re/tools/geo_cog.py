"""geo_cog.py -- read a WINDOW of a remote cloud-optimised GeoTIFF without GDAL.

The GEO TRACK pipeline has tifffile and numpy but no GDAL/rasterio, and the
two land-cover layers it wants -- ESA WorldCover (10 m) and the Meta/WRI
Global Canopy Height map (1 m) -- are published as COGs on AWS S3. A COG is a
tiled GeoTIFF whose header and tile index come first, so a small window costs
the header plus the few tiles it touches, fetched with HTTP Range requests.

Every byte range fetched is cached on disk under the place's _cache, keyed by
URL + range, so a place is read off the network once (standing rule: network
only with Mariano's OK, then never again).

    cog = RemoteCOG(url, cache_dir)
    arr, (x0, y0) = cog.read_window(px0, py0, px1, py1)   # pixel window
    lon, lat = cog.pixel_to_lonlat(px, py)                # georeferencing
"""
from __future__ import annotations

import hashlib
import io
import os
import urllib.request

import numpy as np
import tifffile

USER_AGENT = "TD5RE-geo/1.0 (offline cache builder; contact via repo)"


def _cached_range(url: str, start: int, length: int, cache_dir: str) -> bytes:
    key = hashlib.sha1(("%s|%d|%d" % (url, start, length)).encode()).hexdigest()[:24]
    path = os.path.join(cache_dir, "cog_" + key + ".bin")
    if os.path.exists(path) and os.path.getsize(path) == length:
        with open(path, "rb") as f:
            return f.read()
    req = urllib.request.Request(url, headers={
        "Range": "bytes=%d-%d" % (start, start + length - 1),
        "User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=120) as r:
        data = r.read()
    if len(data) != length:
        raise IOError("short range read %d of %d from %s" % (len(data), length, url))
    os.makedirs(cache_dir, exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    return data


class _RangeFile(io.RawIOBase):
    """Seekable read-only file over HTTP Range, block-cached, for tifffile's
    header/IFD parse (which reads small scattered pieces)."""

    BLOCK = 1 << 16

    def __init__(self, url: str, cache_dir: str, size: int):
        self.url, self.cache_dir, self.size, self.pos = url, cache_dir, size, 0
        self._blocks: dict[int, bytes] = {}

    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, off, whence=0):
        self.pos = off if whence == 0 else (self.pos + off if whence == 1 else self.size + off)
        return self.pos

    def _block(self, i: int) -> bytes:
        if i not in self._blocks:
            start = i * self.BLOCK
            n = min(self.BLOCK, self.size - start)
            self._blocks[i] = _cached_range(self.url, start, n, self.cache_dir)
        return self._blocks[i]

    def read(self, n=-1):
        if n is None or n < 0:
            n = self.size - self.pos
        out = bytearray()
        while n > 0 and self.pos < self.size:
            b = self._block(self.pos // self.BLOCK)
            o = self.pos % self.BLOCK
            take = min(n, len(b) - o)
            out += b[o:o + take]
            self.pos += take
            n -= take
        return bytes(out)

    def readinto(self, buf):
        d = self.read(len(buf))
        buf[:len(d)] = d
        return len(d)


def _remote_size(url: str, cache_dir: str) -> int:
    path = os.path.join(cache_dir, "cog_size_" +
                        hashlib.sha1(url.encode()).hexdigest()[:24] + ".txt")
    if os.path.exists(path):
        return int(open(path).read().strip())
    req = urllib.request.Request(url, method="HEAD", headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=60) as r:
        size = int(r.headers["Content-Length"])
    os.makedirs(cache_dir, exist_ok=True)
    with open(path, "w") as f:
        f.write(str(size))
    return size


class RemoteCOG:
    MAX_RUN = 16 << 20        # one Range request covers at most 16 MB
    def __init__(self, url: str, cache_dir: str):
        self.url, self.cache_dir = url, cache_dir
        self.size = _remote_size(url, cache_dir)
        self._fh = _RangeFile(url, cache_dir, self.size)
        self.tif = tifffile.TiffFile(self._fh)
        self.page = self.tif.pages[0]
        tags = self.page.tags
        scale = tags["ModelPixelScaleTag"].value
        tie = tags["ModelTiepointTag"].value
        self.px_w, self.px_h = float(scale[0]), float(scale[1])
        self.x0, self.y0 = float(tie[3]), float(tie[4])
        self.width, self.height = self.page.imagewidth, self.page.imagelength
        self.tw, self.th = self.page.tilewidth, self.page.tilelength
        self.tiled = bool(self.tw and self.th)
        if not self.tiled:                    # stripped: one strip = full rows
            self.tw = self.width
            self.th = int(self.page.rowsperstrip or 1)
        self.epsg = None
        gk = tags.get("GeoKeyDirectoryTag")
        if gk is not None:
            v = list(gk.value)
            for i in range(4, len(v), 4):
                if v[i] in (2048, 3072):          # Geographic / Projected CRS
                    self.epsg = v[i + 3]

    def describe(self) -> dict:
        return {"url": self.url, "bytes": self.size, "width": self.width,
                "height": self.height, "tile": [self.tw, self.th],
                "compression": str(self.page.compression),
                "tiled": self.tiled, "rows_per_strip": int(self.page.rowsperstrip or 0),
                "predictor": str(self.page.predictor),
                "dtype": str(self.page.dtype), "epsg": self.epsg,
                "pixel": [self.px_w, self.px_h], "origin": [self.x0, self.y0]}

    # georeferencing (north-up)
    def xy_to_pixel(self, x: float, y: float) -> tuple[float, float]:
        return (x - self.x0) / self.px_w, (self.y0 - y) / self.px_h

    def pixel_to_xy(self, px: float, py: float) -> tuple[float, float]:
        return self.x0 + px * self.px_w, self.y0 - py * self.px_h

    def read_window(self, px0: int, py0: int, px1: int, py1: int) -> np.ndarray:
        """Pixels [px0,px1) x [py0,py1), clipped to the image."""
        px0, py0 = max(0, px0), max(0, py0)
        px1, py1 = min(self.width, px1), min(self.height, py1)
        out = np.zeros((py1 - py0, px1 - px0), dtype=self.page.dtype)
        tx_n = (self.width + self.tw - 1) // self.tw
        offs, cnts = self.page.dataoffsets, self.page.databytecounts
        need = [ty * tx_n + tx
                for ty in range(py0 // self.th, (py1 - 1) // self.th + 1)
                for tx in range(px0 // self.tw, (px1 - 1) // self.tw + 1)
                if cnts[ty * tx_n + tx]]
        # Coalesce byte-contiguous blocks into one Range request (stripped
        # COGs are one row per strip: a 5 km window is ~4000 strips).
        raw_of: dict[int, bytes] = {}
        k = 0
        while k < len(need):
            j = k
            while (j + 1 < len(need) and offs[need[j + 1]] == offs[need[j]] + cnts[need[j]]
                   and offs[need[j + 1]] + cnts[need[j + 1]] - offs[need[k]] <= self.MAX_RUN):
                j += 1
            start = offs[need[k]]
            blob = _cached_range(self.url, start, offs[need[j]] + cnts[need[j]] - start,
                                 self.cache_dir)
            for q in range(k, j + 1):
                i = need[q]
                raw_of[i] = blob[offs[i] - start:offs[i] - start + cnts[i]]
            k = j + 1
        for i, raw in raw_of.items():
            ty, tx = divmod(i, tx_n)
            tile, _, _ = self.page.decode(raw, i)
            tile = np.asarray(tile)
            rows = tile.size // self.tw          # last strip may be short
            tile = tile.reshape(rows, self.tw)
            if rows < self.th:
                tile = np.pad(tile, ((0, self.th - rows), (0, 0)))
            gx0, gy0 = tx * self.tw, ty * self.th
            ax0, ay0 = max(px0, gx0), max(py0, gy0)
            ax1, ay1 = min(px1, gx0 + self.tw), min(py1, gy0 + self.th)
            out[ay0 - py0:ay1 - py0, ax0 - px0:ax1 - px0] =                 tile[ay0 - gy0:ay1 - gy0, ax0 - gx0:ax1 - gx0]
        return out
