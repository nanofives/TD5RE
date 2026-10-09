"""geo_buildings_extra.py -- footprints and heights OSM does not have.

La Plata's OSM cache holds 2326 building footprints for a 5.4 x 5.4 km box of a
dense planned city, and a ray-cast from the road centrelines finds a facade
within 25 m on only 3-9% of street sides (memory note 2026-10-08). The city is
there; OSM simply has not traced it. Two open datasets have:

  OVERTURE MAPS, buildings theme (ODbL 1.0)
      OSM + Esri Community Maps + Microsoft ML + Google Open Buildings
      footprints, already conflated and de-duplicated by Overture, with
      height / num_floors / roof attributes where a source carries them.
      Public GeoParquet on S3, one file per spatial partition, read here with
      HTTP range requests: the footer, then only the row groups whose bbox
      statistics touch the place, then only the column chunks we keep.

  GOOGLE OPEN BUILDINGS 2.5D TEMPORAL v1 (CC BY 4.0 or ODbL 1.0; ODbL taken
      to match OSM). Annual 2016-2023 rasters from Sentinel-2, with a
      `building_height` band in metres above terrain, 4 m effective. Read for
      2023 only, from the public GCS bucket, again by range request: the
      per-S2-cell manifest, then the TIFF header, then only the TIFF tiles that
      cover the place, at the coarsest overview finer than OB_TARGET_M.
      Citation: Sirko et al. 2023, "High-Resolution Building and Road
      Detection from Sentinel-2", arXiv:2310.11622. Contains modified
      Copernicus Sentinel-2 data.

NETWORK DISCIPLINE. Every request goes through ONE function, `Session.get`,
which (1) answers from `<place>/_cache/xb_<sha>.bin` when the exact
url+byte-range was fetched before, (2) otherwise refuses unless the session is
online, the host is on the allow-list and the request/byte budget has room,
and (3) writes the raw response to the cache BEFORE anything parses it. So a
parse bug costs a re-run, never a re-fetch, and a dry run with the session
offline names every request it WOULD have made. The parsed products are cached
too (`_cache/xb_overture_<release>_<bbox>.parquet`, `_cache/xb_ob_<year>_<bbox>
.npz`); once they exist the raw responses are never consulted again.

The products are in WGS84 lon/lat. geo_fetch projects them with the place's
own LocalProjection, so the re-normaliser needs neither pyproj nor s2sphere --
only the fetch does (both live in a throwaway venv, not the global Python).
"""
from __future__ import annotations

import hashlib
import io
import json
import math
import os
import time
import urllib.error
import urllib.parse
import urllib.request

import numpy as np

UA = "TD5RE-geo-track/0.1 (+https://github.com/; source port research tool)"

# --- Overture -------------------------------------------------------------
OVERTURE_STAC_ROOT = "https://stac.overturemaps.org/catalog.json"
OVERTURE_COLLECTIONS = "https://stac.overturemaps.org/%s/collections.parquet"
OVERTURE_S3_HTTPS = "https://overturemaps-us-west-2.s3.us-west-2.amazonaws.com/"
# Columns kept from the building type. Everything else (names, version,
# facade_material, roof_direction ...) is either unused by the game or carried
# by OSM where it matters.
OVERTURE_COLUMNS = (
    "id", "geometry", "bbox", "sources", "subtype", "class", "height",
    "num_floors", "min_height", "min_floor", "roof_shape", "roof_height",
    "roof_color", "roof_material", "facade_color", "facade_material",
    "is_underground", "has_parts",
)

# --- Google Open Buildings 2.5D Temporal ---------------------------------
OB_BUCKET = "open-buildings-temporal-data"
OB_VERSION = "v1"
OB_LIST = ("https://storage.googleapis.com/storage/v1/b/%s/o?prefix=%s"
           "&fields=items(name,size),nextPageToken")
OB_OBJECT = "https://storage.googleapis.com/%s/%s"
OB_MANIFEST_S2_LEVEL = 2          # notebook: _MANIFEST_S2_LEVEL = 2
OB_YEAR = 2023                    # the latest year the dataset publishes
OB_HEIGHT_BAND = "building_height"
# Coarsest overview still finer than this is read. The product's EFFECTIVE
# resolution is 4 m (EE catalog), so 2 m keeps all the information at a
# sixteenth of the 0.5 m base's bytes.
OB_TARGET_M = 2.0
# Lat/lon resampling step of the cached product, in metres. Half the effective
# resolution: nothing real is lost, and a 120 m2 house still gets ~30 samples.
OB_PRODUCT_M = 2.0

OB_CITATION = ("Sirko, W. et al. (2023). High-Resolution Building and Road "
               "Detection from Sentinel-2. arXiv:2310.11622")

DEFAULT_HOSTS = (
    "stac.overturemaps.org",
    "overturemaps-us-west-2.s3.us-west-2.amazonaws.com",
    "storage.googleapis.com",
)


class Blocked(RuntimeError):
    """A request the session refused (offline, host, or budget)."""


class Session:
    """The single network gate for this module. See the module docstring."""

    def __init__(self, cache_dir: str, online: bool = False,
                 hosts=DEFAULT_HOSTS, max_requests: int = 400,
                 max_bytes: int = 600 * 1024 * 1024):
        self.cache_dir = cache_dir
        self.online = online
        self.hosts = set(hosts)
        self.max_requests = max_requests
        self.max_bytes = max_bytes
        self.requests = 0
        self.bytes = 0
        self.hits = 0
        self.log: list[dict] = []
        self.would: list[str] = []

    def _path(self, url: str, rng) -> str:
        key = url + "\x00" + ("" if rng is None else "%s-%s" % rng)
        h = hashlib.sha256(key.encode("utf-8")).hexdigest()[:24]
        return os.path.join(self.cache_dir, "xb_%s.bin" % h)

    def get(self, url: str, rng=None) -> tuple[bytes, int | None]:
        """(body, total object size or None). `rng` = (start, end) inclusive,
        or (None, n) for the LAST n bytes."""
        path = self._path(url, rng)
        meta = path + ".json"
        if os.path.exists(path) and os.path.exists(meta):
            with open(path, "rb") as f:
                body = f.read()
            with open(meta, encoding="utf-8") as f:
                total = json.load(f).get("total")
            self.hits += 1
            return body, total
        host = urllib.parse.urlsplit(url).hostname or ""
        desc = "%s %s" % (url, "" if rng is None else "bytes=%s-%s" % (
            "" if rng[0] is None else rng[0], rng[1]))
        if not self.online:
            self.would.append(desc)
            raise Blocked("offline: WOULD FETCH " + desc)
        if host not in self.hosts:
            raise Blocked("host not allowed: " + host)
        want = 0 if rng is None or rng[0] is None else rng[1] - rng[0] + 1
        if self.requests + 1 > self.max_requests or \
                self.bytes + want > self.max_bytes:
            raise Blocked("budget: %d req / %d B used, next %s"
                          % (self.requests, self.bytes, desc))
        hdr = {"User-Agent": UA}
        if rng is not None:
            hdr["Range"] = "bytes=%s-%s" % ("" if rng[0] is None else rng[0],
                                            rng[1])
        last = None
        for attempt in range(3):
            try:
                req = urllib.request.Request(url, headers=hdr)
                with urllib.request.urlopen(req, timeout=180) as r:
                    body = r.read()
                    cr = r.headers.get("Content-Range")
                    total = None
                    if cr and "/" in cr:
                        t = cr.rsplit("/", 1)[1].strip()
                        total = int(t) if t.isdigit() else None
                    elif rng is None:
                        total = len(body)
                    status = r.status
                break
            except (urllib.error.URLError, OSError) as exc:
                last = exc
                if isinstance(exc, urllib.error.HTTPError) and exc.code in (
                        403, 404, 416):
                    break
                time.sleep(3.0 * (attempt + 1))
        else:
            raise RuntimeError("fetch failed: %s (%s)" % (desc, last))
        if last is not None and "body" not in locals():
            raise RuntimeError("fetch failed: %s (%s)" % (desc, last))
        if rng is not None and rng[0] is not None and status != 206:
            # A server that ignored Range sent the whole object. Refuse to
            # keep going rather than silently blowing the byte budget again.
            raise RuntimeError("server ignored Range for " + desc)
        self.requests += 1
        self.bytes += len(body)
        os.makedirs(self.cache_dir, exist_ok=True)
        with open(path, "wb") as f:
            f.write(body)
        with open(meta, "w", encoding="utf-8") as f:
            json.dump({"url": url, "range": rng, "total": total,
                       "bytes": len(body), "status": status,
                       "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                            time.gmtime())}, f)
        self.log.append({"url": url, "range": rng, "bytes": len(body)})
        return body, total

    def summary(self) -> dict:
        return {"requests": self.requests, "bytes": self.bytes,
                "cache_hits": self.hits, "would_fetch": len(self.would)}


class RangeFile(io.RawIOBase):
    """A seekable read-only file over HTTP range requests through a Session.

    Spans already fetched are kept in memory; a read inside one costs nothing.
    `prefetch(spans)` lets the caller coalesce the reads it is about to make
    (a whole row group's column chunks, a run of TIFF tiles) into one request
    each, instead of one request per pyarrow/tifffile read call.
    """

    MIN_READ = 64 * 1024

    def __init__(self, sess: Session, url: str):
        super().__init__()
        self.sess, self.url, self.pos = sess, url, 0
        self.spans: list[tuple[int, bytes]] = []
        tail, total = sess.get(url, (None, self.MIN_READ))
        if total is None:
            raise RuntimeError("no Content-Range total for " + url)
        self.size = total
        self.spans.append((total - len(tail), tail))

    # io plumbing
    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, off, whence=0):
        if whence == 0:
            self.pos = off
        elif whence == 1:
            self.pos += off
        else:
            self.pos = self.size + off
        return self.pos

    def _covered(self, a: int, b: int) -> bytes | None:
        for s, data in self.spans:
            if s <= a and b <= s + len(data):
                return data[a - s:b - s]
        return None

    def fetch(self, a: int, b: int) -> None:
        """Make [a, b) resident (one request unless already covered)."""
        b = min(b, self.size)
        if a >= b or self._covered(a, b) is not None:
            return
        body, _ = self.sess.get(self.url, (a, b - 1))
        self.spans.append((a, body))

    def prefetch(self, spans, gap: int = 256 * 1024) -> None:
        spans = sorted((a, b) for a, b in spans if b > a)
        merged: list[list[int]] = []
        for a, b in spans:
            if merged and a <= merged[-1][1] + gap:
                merged[-1][1] = max(merged[-1][1], b)
            else:
                merged.append([a, b])
        for a, b in merged:
            self.fetch(a, b)

    def read(self, n=-1):
        if n is None or n < 0:
            n = self.size - self.pos
        a, b = self.pos, min(self.size, self.pos + n)
        if a >= b:
            return b""
        got = self._covered(a, b)
        if got is None:
            self.fetch(a, max(b, min(self.size, a + self.MIN_READ)))
            got = self._covered(a, b)
        self.pos = b
        return got

    def readinto(self, buf):
        data = self.read(len(buf))
        buf[:len(data)] = data
        return len(data)


def bbox_key(bbox) -> str:
    s, w, n, e = bbox
    return hashlib.sha256(("%.6f,%.6f,%.6f,%.6f" % (s, w, n, e))
                          .encode()).hexdigest()[:12]


# =============================================================== Overture ==

def overture_latest_release(sess: Session) -> str:
    body, _ = sess.get(OVERTURE_STAC_ROOT)
    return json.loads(body)["latest"]


def _overture_files(sess: Session, release: str, bbox) -> list[str]:
    """HTTPS urls of the building GeoParquet files whose extent meets bbox."""
    import pyarrow as pa
    import pyarrow.parquet as pq
    s, w, n, e = bbox
    body, _ = sess.get(OVERTURE_COLLECTIONS % release)
    t = pq.read_table(pa.BufferReader(body)).to_pylist()
    out = []
    for row in t:
        blob = json.dumps(row, default=str)
        if "type=building/" not in blob:
            continue
        bb = row.get("bbox")
        if isinstance(bb, dict):
            x0, y0, x1, y1 = bb["xmin"], bb["ymin"], bb["xmax"], bb["ymax"]
        elif isinstance(bb, (list, tuple)) and len(bb) >= 4:
            x0, y0, x1, y1 = bb[0], bb[1], bb[2], bb[3]
        else:
            continue
        if x0 > e or x1 < w or y0 > n or y1 < s:
            continue
        # The asset href: the S3 (or https S3) form of the parquet file.
        href = None
        for tok in blob.replace('"', " ").split():
            tok = tok.strip(",}]")
            if "type=building/" in tok and tok.endswith(".parquet"):
                href = tok
                break
        if href is None:
            continue
        if href.startswith("s3://overturemaps-us-west-2/"):
            href = OVERTURE_S3_HTTPS + href[len("s3://overturemaps-us-west-2/"):]
        if href not in out:
            out.append(href)
    return out


def _rg_hits(md, rg: int, bbox) -> bool:
    """Row group `rg`'s bbox statistics meet the place bbox."""
    s, w, n, e = bbox
    lo, hi = {}, {}
    g = md.row_group(rg)
    for c in range(g.num_columns):
        col = g.column(c)
        p = col.path_in_schema
        if p in ("bbox.xmin", "bbox.ymin", "bbox.xmax", "bbox.ymax"):
            st = col.statistics
            if st is None or not st.has_min_max:
                return True                     # no stats: cannot prune
            lo[p], hi[p] = st.min, st.max
    if len(lo) < 4:
        return True
    return not (lo["bbox.xmin"] > e or hi["bbox.xmax"] < w or
                lo["bbox.ymin"] > n or hi["bbox.ymax"] < s)


def overture_plan(sess: Session, bbox, release: str | None = None) -> dict:
    """Metadata only: release, files, selected row groups, and the exact bytes
    the data step will read. Cheap (catalog + index + one footer per file)."""
    import pyarrow as pa
    import pyarrow.parquet as pq
    release = release or overture_latest_release(sess)
    files = _overture_files(sess, release, bbox)
    plan = {"release": release, "files": []}
    for url in files:
        rf = RangeFile(sess, url)
        pf = pq.ParquetFile(pa.PythonFile(rf, mode="r"))
        md = pf.metadata
        names = set(pf.schema_arrow.names)
        cols = [c for c in OVERTURE_COLUMNS if c in names]
        rgs = [i for i in range(md.num_row_groups) if _rg_hits(md, i, bbox)]
        spans = []
        for i in rgs:
            g = md.row_group(i)
            for c in range(g.num_columns):
                col = g.column(c)
                if col.path_in_schema.split(".")[0] not in cols:
                    continue
                start = col.dictionary_page_offset or col.data_page_offset
                if col.dictionary_page_offset is not None:
                    start = min(start, col.data_page_offset)
                spans.append((start, start + col.total_compressed_size))
        plan["files"].append({
            "url": url, "size": rf.size, "row_groups": md.num_row_groups,
            "rows": md.num_rows, "selected": rgs,
            "selected_rows": sum(md.row_group(i).num_rows for i in rgs),
            "columns": cols, "spans": spans,
            "bytes": sum(b - a for a, b in spans)})
    plan["bytes"] = sum(f["bytes"] for f in plan["files"])
    return plan


def overture_fetch(sess: Session, bbox, plan: dict, out_path: str) -> dict:
    """Read the planned row groups, keep the rows inside bbox, write a small
    GeoParquet-ish product (WKB geometry + the kept columns)."""
    import pyarrow as pa
    import pyarrow.compute as pc
    import pyarrow.parquet as pq
    s, w, n, e = bbox
    tables = []
    for f in plan["files"]:
        if not f["selected"]:
            continue
        rf = RangeFile(sess, f["url"])
        rf.prefetch([tuple(x) for x in f["spans"]])
        pf = pq.ParquetFile(pa.PythonFile(rf, mode="r"))
        t = pf.read_row_groups(f["selected"], columns=f["columns"])
        bb = t.column("bbox")
        m = pc.and_(pc.and_(pc.less_equal(pc.struct_field(bb, "xmin"), e),
                            pc.greater_equal(pc.struct_field(bb, "xmax"), w)),
                    pc.and_(pc.less_equal(pc.struct_field(bb, "ymin"), n),
                            pc.greater_equal(pc.struct_field(bb, "ymax"), s)))
        tables.append(t.filter(m))
    if tables:
        t = pa.concat_tables(tables, promote_options="permissive")
    else:
        t = pa.table({"id": pa.array([], pa.string())})
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    pq.write_table(t, out_path + ".tmp", compression="zstd")
    os.replace(out_path + ".tmp", out_path)
    return {"rows": t.num_rows}


# ======================================================== Open Buildings ==

def _s2_tokens(bbox) -> list[str]:
    import s2sphere as s2
    s, w, n, e = bbox
    r = s2.LatLngRect.from_point_pair(s2.LatLng.from_degrees(s, w),
                                      s2.LatLng.from_degrees(n, e))
    cov = s2.RegionCoverer()
    cov.min_level = cov.max_level = OB_MANIFEST_S2_LEVEL
    cov.max_cells = 1000000
    return [c.to_token() for c in cov.get_covering(r)]


def _gs_to_https(uri: str) -> str:
    if uri.startswith("gs://"):
        b, _, k = uri[5:].partition("/")
        return OB_OBJECT % (b, urllib.parse.quote(k))
    return uri


def ob_manifests(sess: Session, bbox, year: int = OB_YEAR) -> list[dict]:
    out = []
    for tok in _s2_tokens(bbox):
        prefix = "%s/manifests/%s_" % (OB_VERSION, tok)
        url = OB_LIST % (OB_BUCKET, urllib.parse.quote(prefix, safe=""))
        page = None
        while True:
            body, _ = sess.get(url + ("" if page is None else
                                      "&pageToken=" + page))
            j = json.loads(body)
            for it in j.get("items", []):
                if "_%d_" % year in it["name"]:
                    mb, _ = sess.get(OB_OBJECT % (OB_BUCKET, urllib.parse.quote(
                        it["name"])))
                    out.append({"name": it["name"], "manifest": json.loads(mb)})
            page = j.get("nextPageToken")
            if not page:
                break
    return out


def _ob_sources(man: dict):
    """(crs, uri, affine, (w,h), tileset band index of OB_HEIGHT_BAND)."""
    prefix = man.get("uriPrefix", "")
    bands = {b.get("tilesetId", ""): b for b in man.get("bands", [])
             if b.get("id") == OB_HEIGHT_BAND}
    for ts in man.get("tilesets", []):
        tid = ts.get("id", "")
        if bands and tid not in bands and "" not in bands:
            continue
        bi = (bands.get(tid) or bands.get("") or {}).get("tilesetBandIndex", 0)
        crs = ts.get("crs")
        for src in ts.get("sources", []):
            at = src.get("affineTransform", ts.get("affineTransform"))
            dim = src.get("dimensions", ts.get("dimensions"))
            for u in src.get("uris", []):
                yield crs, prefix + u, at, dim, bi


def ob_plan(sess: Session, bbox, year: int = OB_YEAR) -> dict:
    """Manifests -> the GeoTIFFs that meet bbox -> per TIFF the overview level
    and tile byte spans the data step will read."""
    import pyproj
    import tifffile
    s, w, n, e = bbox
    plan = {"year": year, "tiffs": [], "manifests": []}
    seen = set()
    for m in ob_manifests(sess, bbox, year):
        plan["manifests"].append(m["name"])
        for crs, uri, at, dim, bi in _ob_sources(m["manifest"]):
            url = _gs_to_https(uri)
            if url in seen:
                continue
            tr = pyproj.Transformer.from_crs("EPSG:4326", crs, always_xy=True)
            xs, ys = tr.transform([w, e, e, w], [s, s, n, n])
            bx0, bx1, by0, by1 = min(xs), max(xs), min(ys), max(ys)
            if at and dim:
                tx0, sx = at["translateX"], at["scaleX"]
                ty0, sy = at["translateY"], at["scaleY"]
                tx1 = tx0 + sx * dim["width"]
                ty1 = ty0 + sy * dim["height"]
                if max(tx0, tx1) < bx0 or min(tx0, tx1) > bx1 or \
                        max(ty0, ty1) < by0 or min(ty0, ty1) > by1:
                    continue
            seen.add(url)
            rf = RangeFile(sess, url)
            # The IFDs sit at the front of a COG; read the head generously so
            # tifffile's IFD walk is one request, not dozens.
            rf.fetch(0, 1 << 20)
            with tifffile.TiffFile(rf) as tf:
                levels = [tf.series[0].levels[i] if hasattr(tf.series[0],
                          "levels") else tf.series[0]
                          for i in range(len(getattr(tf.series[0], "levels",
                                                     [tf.series[0]])))]
                base = tf.pages[0]
                bw, bh = base.imagewidth, base.imagelength
                sx_base = (at["scaleX"] if at else None)
                pick = 0
                for li, lv in enumerate(levels):
                    pg = lv.pages[0] if hasattr(lv, "pages") else lv
                    f = bw / pg.imagewidth
                    if sx_base is not None and abs(sx_base) * f <= OB_TARGET_M:
                        pick = li
                lv = levels[pick]
                pg = lv.pages[0] if hasattr(lv, "pages") else lv
                f = bw / pg.imagewidth
                px = abs(at["scaleX"]) * f
                py = abs(at["scaleY"]) * f
                # Pixel window of bbox at this level.
                c0 = int(math.floor((bx0 - at["translateX"]) / px)) - 2
                c1 = int(math.ceil((bx1 - at["translateX"]) / px)) + 2
                if at["scaleY"] < 0:
                    r0 = int(math.floor((at["translateY"] - by1) / py)) - 2
                    r1 = int(math.ceil((at["translateY"] - by0) / py)) + 2
                else:
                    r0 = int(math.floor((by0 - at["translateY"]) / py)) - 2
                    r1 = int(math.ceil((by1 - at["translateY"]) / py)) + 2
                c0, r0 = max(0, c0), max(0, r0)
                c1, r1 = min(pg.imagewidth, c1), min(pg.imagelength, r1)
                tw, tl = pg.tilewidth, pg.tilelength
                if not tw:
                    raise RuntimeError("Open Buildings TIFF is not tiled: "
                                       + url)
                ntx = (pg.imagewidth + tw - 1) // tw
                spp = pg.samplesperpixel
                planar = pg.planarconfig == 2
                ntiles_plane = ntx * ((pg.imagelength + tl - 1) // tl)
                idx = []
                for ty in range(r0 // tl, (r1 - 1) // tl + 1):
                    for tx in range(c0 // tw, (c1 - 1) // tw + 1):
                        t = ty * ntx + tx
                        idx.append(t + (bi * ntiles_plane if planar else 0))
                spans = [(pg.dataoffsets[i],
                          pg.dataoffsets[i] + pg.databytecounts[i])
                         for i in idx if pg.databytecounts[i]]
                plan["tiffs"].append({
                    "url": url, "crs": crs, "size": rf.size,
                    "affine": at, "dims": dim, "band_index": bi,
                    "samples": spp, "planar": planar, "dtype": str(pg.dtype),
                    "level": pick, "levels": len(levels), "pixel_m": [px, py],
                    "window": [c0, r0, c1, r1], "tile": [tw, tl],
                    "tiles": idx, "spans": spans,
                    "bytes": sum(b - a for a, b in spans),
                    "nodata": getattr(pg, "nodata", None),
                    "compression": str(pg.compression)})
    plan["bytes"] = sum(t["bytes"] for t in plan["tiffs"])
    return plan


def ob_fetch(sess: Session, bbox, plan: dict, out_path: str) -> dict:
    """Read the planned tiles and resample the height band onto a regular
    lon/lat grid of OB_PRODUCT_M spacing over bbox (nearest). NaN = no tile."""
    import pyproj
    import tifffile
    s, w, n, e = bbox
    lat_c = 0.5 * (s + n)
    dlat = OB_PRODUCT_M / 110_574.0
    dlon = OB_PRODUCT_M / (111_320.0 * math.cos(math.radians(lat_c)))
    ny = int(math.ceil((n - s) / dlat)) + 1
    nx = int(math.ceil((e - w) / dlon)) + 1
    grid = np.full((ny, nx), np.nan, np.float32)
    lats = n - np.arange(ny) * dlat           # row 0 = north
    lons = w + np.arange(nx) * dlon
    LON, LAT = np.meshgrid(lons, lats)
    stats = {"tiffs": 0, "tiles": 0}
    for t in plan["tiffs"]:
        rf = RangeFile(sess, t["url"])
        rf.fetch(0, 1 << 20)
        rf.prefetch([tuple(x) for x in t["spans"]])
        with tifffile.TiffFile(rf) as tf:
            ser = tf.series[0]
            lv = ser.levels[t["level"]] if hasattr(ser, "levels") else ser
            pg = lv.pages[0] if hasattr(lv, "pages") else lv
            c0, r0, c1, r1 = t["window"]
            tw, tl = t["tile"]
            ntx = (pg.imagewidth + tw - 1) // tw
            ntiles_plane = ntx * ((pg.imagelength + tl - 1) // tl)
            win = np.full((r1 - r0, c1 - c0), np.nan, np.float32)
            fh = tf.filehandle
            for i in t["tiles"]:
                off, cnt = pg.dataoffsets[i], pg.databytecounts[i]
                if not cnt:
                    continue
                fh.seek(off)
                data = fh.read(cnt)
                arr, _, _ = pg.decode(data, i)
                arr = np.asarray(arr)
                # tifffile returns (1, tl, tw, spp) for a chunky tile.
                arr = arr.reshape(arr.shape[-3], arr.shape[-2], -1) \
                    if arr.ndim >= 3 else arr[..., None]
                band = arr[..., 0 if t["planar"] else t["band_index"]]
                ti = i - (t["band_index"] * ntiles_plane if t["planar"] else 0)
                ty, tx = divmod(ti, ntx)
                y0, x0 = ty * tl, tx * tw
                ys0, xs0 = max(y0, r0), max(x0, c0)
                ys1 = min(y0 + band.shape[0], r1)
                xs1 = min(x0 + band.shape[1], c1)
                if ys1 <= ys0 or xs1 <= xs0:
                    continue
                win[ys0 - r0:ys1 - r0, xs0 - c0:xs1 - c0] = \
                    band[ys0 - y0:ys1 - y0, xs0 - x0:xs1 - x0]
                stats["tiles"] += 1
            nd = t.get("nodata")
            if nd is not None:
                win[win == nd] = np.nan
        tr = pyproj.Transformer.from_crs("EPSG:4326", t["crs"], always_xy=True)
        X, Y = tr.transform(LON, LAT)
        at = t["affine"]
        px, py = t["pixel_m"]
        col = np.floor((X - at["translateX"]) / px).astype(np.int64) - c0
        if at["scaleY"] < 0:
            row = np.floor((at["translateY"] - Y) / py).astype(np.int64) - r0
        else:
            row = np.floor((Y - at["translateY"]) / py).astype(np.int64) - r0
        ok = (col >= 0) & (col < win.shape[1]) & (row >= 0) & \
             (row < win.shape[0])
        vals = np.full(grid.shape, np.nan, np.float32)
        vals[ok] = win[row[ok], col[ok]]
        fill = np.isnan(grid) & ~np.isnan(vals)
        grid[fill] = vals[fill]
        stats["tiffs"] += 1
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    np.savez_compressed(out_path + ".tmp.npz", height=grid,
                        north=n, west=w, dlat=dlat, dlon=dlon, year=plan["year"])
    os.replace(out_path + ".tmp.npz", out_path)
    stats["valid_cells"] = int((~np.isnan(grid)).sum())
    stats["cells"] = int(grid.size)
    return stats


# ================================================================ driver ==

def products(place_out: str, bbox, release: str | None = None) -> dict:
    """Paths of the cached products for this place (existing or not)."""
    cache = os.path.join(place_out, "_cache")
    key = bbox_key(bbox)
    prov_path = os.path.join(cache, "xb_provenance_%s.json" % key)
    prov = {}
    if os.path.exists(prov_path):
        with open(prov_path, encoding="utf-8") as f:
            prov = json.load(f)
    rel = release or prov.get("overture", {}).get("release")
    return {
        "provenance": prov_path, "prov": prov,
        "overture": (os.path.join(cache, "xb_overture_%s_%s.parquet"
                                  % (rel, key)) if rel else None),
        "ob": os.path.join(cache, "xb_ob_%d_%s.npz" % (OB_YEAR, key)),
    }


def fetch_extra(place_out: str, bbox, online: bool, release: str | None = None,
                max_bytes: int = 600 * 1024 * 1024,
                plan_only: bool = False) -> dict:
    """The one fetch session. Offline (default) = a dry run that names every
    request it would make; nothing parses past the first missing response."""
    cache = os.path.join(place_out, "_cache")
    sess = Session(cache, online=online, max_bytes=max_bytes)
    p = products(place_out, bbox, release)
    if p["overture"] and os.path.exists(p["overture"]) and \
            os.path.exists(p["ob"]):
        print("  extra buildings: cached products present, no network")
        return p["prov"]
    prov = {"bbox": list(bbox), "started_utc": time.strftime(
        "%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    print("  [overture] plan")
    op = overture_plan(sess, bbox, release)
    print("    release %s, %d file(s), %d row groups selected, %d rows, "
          "%.1f MB to read" % (
              op["release"], len(op["files"]),
              sum(len(f["selected"]) for f in op["files"]),
              sum(f["selected_rows"] for f in op["files"]),
              op["bytes"] / 1e6))
    print("  [open buildings] plan")
    obp = ob_plan(sess, bbox)
    for t in obp["tiffs"]:
        print("    %s level %d/%d (%.2f m px), %d tiles, %.1f MB, %s %s" % (
            t["url"].rsplit("/", 1)[-1], t["level"], t["levels"],
            t["pixel_m"][0], len(t["tiles"]), t["bytes"] / 1e6, t["dtype"],
            t["compression"]))
    print("    manifests %s, %.1f MB to read" % (obp["manifests"],
                                                 obp["bytes"] / 1e6))
    meta = sess.summary()
    print("  metadata: %d request(s), %.2f MB, %d cache hit(s)"
          % (meta["requests"], meta["bytes"] / 1e6, meta["cache_hits"]))
    planned = op["bytes"] + obp["bytes"]
    if sess.bytes + planned > max_bytes:
        raise Blocked("data step needs %.1f MB, budget %.1f MB"
                      % (planned / 1e6, max_bytes / 1e6))
    plan_dump = os.path.join(cache, "xb_plan_%s.json" % bbox_key(bbox))
    with open(plan_dump, "w", encoding="utf-8") as f:
        json.dump({"overture": op, "open_buildings": obp}, f, indent=1,
                  default=str)
    if plan_only:
        return {"plan": plan_dump, "session": sess.summary()}
    p = products(place_out, bbox, op["release"])
    print("  [overture] data")
    ost = overture_fetch(sess, bbox, op, p["overture"])
    print("    %d buildings in bbox" % ost["rows"])
    print("  [open buildings] data")
    obs = ob_fetch(sess, bbox, obp, p["ob"])
    print("    %d tiles, %d/%d grid cells valid" % (obs["tiles"],
                                                    obs["valid_cells"],
                                                    obs["cells"]))
    prov.update({
        "finished_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "overture": {"release": op["release"], "rows": ost["rows"],
                     "files": [f["url"] for f in op["files"]],
                     "licence": "ODbL 1.0"},
        "open_buildings": {"dataset": "Google Open Buildings 2.5D Temporal v1",
                           "year": OB_YEAR, "band": OB_HEIGHT_BAND,
                           "manifests": obp["manifests"],
                           "tiffs": [t["url"] for t in obp["tiffs"]],
                           "pixel_m": [t["pixel_m"] for t in obp["tiffs"]],
                           "product_m": OB_PRODUCT_M,
                           "licence": "ODbL 1.0 (dual CC BY 4.0 / ODbL; "
                                      "ODbL chosen)",
                           "citation": OB_CITATION,
                           **obs},
        "session": sess.summary(),
        "requests": sess.log,
    })
    with open(p["provenance"], "w", encoding="utf-8") as f:
        json.dump(prov, f, indent=1)
    s = sess.summary()
    print("  session: %d request(s), %d bytes (%.2f MB), %d cache hit(s)"
          % (s["requests"], s["bytes"], s["bytes"] / 1e6, s["cache_hits"]))
    return prov
