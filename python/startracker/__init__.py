# SPDX-License-Identifier: Apache-2.0
"""Python bindings (ctypes) for the startracker-cpp C API (libstartracker).

    import startracker as st
    db = open("esa_default.stdb", "rb").read()
    t = st.Tracker(db, st.Camera.ideal(1280, 960, fov_deg=11.4))
    sol = t.track_image(pixels, t_ns=0)            # numpy uint8/uint16 array or bytes
    cents = t.extract_centroids(pixels)
    res = t.solve_lost_in_space(cents, 1280, 960, fov_estimate_deg=11.4)

The library is found via $STARTRACKER_LIB, then next to this package, then the system loader.
No NumPy dependency; arrays are accepted through the buffer protocol.
"""
import ctypes
import ctypes.util
import os

__all__ = ["Camera", "Tracker", "Solution", "LisResult", "StarTrackerError", "version", "library"]


def _load():
    names = []
    if os.environ.get("STARTRACKER_LIB"):
        names.append(os.environ["STARTRACKER_LIB"])
    here = os.path.dirname(os.path.abspath(__file__))
    for n in ("libstartracker.so", "libstartracker.dylib"):
        names.append(os.path.join(here, n))
    found = ctypes.util.find_library("startracker")
    if found:
        names.append(found)
    for n in names:
        if os.path.exists(n) or not os.path.isabs(n):
            try:
                return ctypes.CDLL(n)
            except OSError:
                pass
    raise OSError("libstartracker not found; set STARTRACKER_LIB")


library = _load()

STATUS = ["OK", "INVALID_ARGUMENT", "CAPACITY_EXCEEDED", "ARENA_EXHAUSTED", "NOT_INITIALIZED",
          "CORRUPT_DATA", "CHECKSUM_MISMATCH", "VERSION_MISMATCH", "UNSUPPORTED", "TOO_FEW_STARS",
          "NO_MATCH", "TIMEOUT", "WORK_BUDGET_EXHAUSTED", "CANCELLED", "IO_ERROR"]
MODES = ["lost", "lost_in_space", "tracking"]
MONO8, MONO16 = 0, 1
VARIANTS = {"esa": 0, "cedar": 1}  # lost-in-space search: ESA tetra3 or cedar-solve


def _variant(name):
    if name not in VARIANTS:
        raise ValueError(f"variant must be one of {sorted(VARIANTS)}, not {name!r}")
    return VARIANTS[name]


class StarTrackerError(RuntimeError):
    def __init__(self, status, what):
        self.status = status
        super().__init__(f"{what}: {library.st_status_string(status).decode()}")


class _Camera(ctypes.Structure):
    _fields_ = [("width", ctypes.c_uint32), ("height", ctypes.c_uint32),
                ("fx", ctypes.c_double), ("fy", ctypes.c_double), ("cx", ctypes.c_double),
                ("cy", ctypes.c_double), ("k1", ctypes.c_double), ("k2", ctypes.c_double),
                ("k3", ctypes.c_double), ("p1", ctypes.c_double), ("p2", ctypes.c_double),
                ("mirrored", ctypes.c_int)]


class _Config(ctypes.Structure):
    _fields_ = [("camera", _Camera), ("max_centroids", ctypes.c_uint32),
                ("roi_half_size", ctypes.c_uint32), ("max_rois", ctypes.c_uint32),
                ("roi_tracking", ctypes.c_int), ("centroid_sigma", ctypes.c_float),
                ("centroid_min_area", ctypes.c_uint32), ("centroid_max_area", ctypes.c_uint32),
                ("match_radius_px", ctypes.c_double), ("min_matches", ctypes.c_uint32),
                ("rate_window", ctypes.c_uint32), ("variant", ctypes.c_uint32)]


class _Centroid(ctypes.Structure):
    _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double)]


class _Solution(ctypes.Structure):
    _fields_ = [("status", ctypes.c_int), ("mode", ctypes.c_int), ("t_ns", ctypes.c_int64),
                ("q_cam_icrs", ctypes.c_double * 4), ("covariance", ctypes.c_double * 9),
                ("rate", ctypes.c_double * 3), ("rate_valid", ctypes.c_int),
                ("matches", ctypes.c_uint32), ("rmse_arcsec", ctypes.c_double),
                ("ra_deg", ctypes.c_double), ("dec_deg", ctypes.c_double), ("roll_deg", ctypes.c_double),
                ("num_centroids", ctypes.c_uint32), ("used_rois", ctypes.c_int)]


class _LisOptions(ctypes.Structure):
    _fields_ = [("fov_estimate_deg", ctypes.c_double), ("fov_max_error_deg", ctypes.c_double),
                ("pattern_checking_stars", ctypes.c_uint32), ("match_radius", ctypes.c_double),
                ("match_threshold", ctypes.c_double), ("distortion", ctypes.c_double),
                ("variant", ctypes.c_uint32)]


class _LisResult(ctypes.Structure):
    _fields_ = [("status", ctypes.c_int), ("ra_deg", ctypes.c_double), ("dec_deg", ctypes.c_double),
                ("roll_deg", ctypes.c_double), ("fov_deg", ctypes.c_double),
                ("rmse_arcsec", ctypes.c_double), ("p90e_arcsec", ctypes.c_double),
                ("maxe_arcsec", ctypes.c_double), ("probability", ctypes.c_double),
                ("num_matches", ctypes.c_uint32), ("q_cam_icrs", ctypes.c_double * 4),
                ("solve_ms", ctypes.c_double)]


_P = ctypes.c_void_p
_sigs = {
    "st_version": (ctypes.c_char_p, []),
    "st_status_string": (ctypes.c_char_p, [ctypes.c_int]),
    "st_config_init": (None, [ctypes.POINTER(_Config)]),
    "st_camera_ideal": (_Camera, [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_double]),
    "st_tracker_memory_size": (ctypes.c_size_t, [ctypes.POINTER(_Config), _P, ctypes.c_size_t]),
    "st_tracker_create": (ctypes.c_int, [ctypes.POINTER(_Config), _P, ctypes.c_size_t, _P, ctypes.c_size_t,
                                         ctypes.POINTER(_P)]),
    "st_track_image": (ctypes.c_int, [_P, ctypes.c_int64, _P, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_size_t,
                                      ctypes.c_int, ctypes.POINTER(_Solution)]),
    "st_track_centroids": (ctypes.c_int, [_P, ctypes.c_int64, ctypes.POINTER(_Centroid), ctypes.c_uint32,
                                          ctypes.POINTER(_Solution)]),
    "st_extract_centroids": (ctypes.c_int, [_P, _P, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_size_t, ctypes.c_int,
                                            ctypes.POINTER(_Centroid), ctypes.c_uint32,
                                            ctypes.POINTER(ctypes.c_uint32)]),
    "st_solve_lost_in_space": (ctypes.c_int, [_P, ctypes.POINTER(_Centroid), ctypes.c_uint32, ctypes.c_uint32,
                                              ctypes.c_uint32, ctypes.POINTER(_LisOptions),
                                              ctypes.POINTER(_LisResult)]),
    "st_tracker_set_attitude": (None, [_P, ctypes.c_int64, ctypes.POINTER(ctypes.c_double)]),
    "st_tracker_reset": (None, [_P]),
}
for _n, (_r, _a) in _sigs.items():
    _f = getattr(library, _n)
    _f.restype, _f.argtypes = _r, _a


def version():
    return library.st_version().decode()


class Camera:
    """Pinhole + Brown-Conrady camera (pixels)."""

    def __init__(self, width, height, fx, fy=None, cx=None, cy=None, k1=0.0, k2=0.0, k3=0.0, p1=0.0, p2=0.0,
                 mirrored=False):
        self.width, self.height, self.fx = int(width), int(height), float(fx)
        self.fy = float(fx if fy is None else fy)
        self.cx = float(width / 2 if cx is None else cx)
        self.cy = float(height / 2 if cy is None else cy)
        self.k1, self.k2, self.k3, self.p1, self.p2, self.mirrored = k1, k2, k3, p1, p2, mirrored

    @classmethod
    def ideal(cls, width, height, fov_deg):
        c = library.st_camera_ideal(width, height, fov_deg)
        return cls(c.width, c.height, c.fx, c.fy, c.cx, c.cy)

    def _c(self):
        return _Camera(self.width, self.height, self.fx, self.fy, self.cx, self.cy, self.k1, self.k2, self.k3,
                       self.p1, self.p2, int(bool(self.mirrored)))


class Solution:
    """Attitude for one frame. q_cam_icrs is (w, x, y, z); covariance is 3x3 in rad^2."""

    def __init__(self, s):
        self.status = STATUS[s.status] if 0 <= s.status < len(STATUS) else str(s.status)
        self.ok = s.status == 0
        self.mode = MODES[s.mode]
        self.t_ns = s.t_ns
        self.q_cam_icrs = tuple(s.q_cam_icrs)
        self.covariance = [list(s.covariance[i * 3:i * 3 + 3]) for i in range(3)]
        self.rate = tuple(s.rate) if s.rate_valid else None
        self.matches = s.matches
        self.rmse_arcsec = s.rmse_arcsec
        self.ra_deg, self.dec_deg, self.roll_deg = s.ra_deg, s.dec_deg, s.roll_deg
        self.num_centroids = s.num_centroids
        self.used_rois = bool(s.used_rois)

    def __repr__(self):
        return (f"Solution({self.status}, {self.mode}, ra={self.ra_deg:.5f}, dec={self.dec_deg:.5f}, "
                f"roll={self.roll_deg:.5f}, matches={self.matches})")


class LisResult:
    def __init__(self, r):
        self.status = STATUS[r.status] if 0 <= r.status < len(STATUS) else str(r.status)
        self.ok = r.status == 0
        for k in ("ra_deg", "dec_deg", "roll_deg", "fov_deg", "rmse_arcsec", "p90e_arcsec", "maxe_arcsec",
                  "probability", "num_matches", "solve_ms"):
            setattr(self, k, getattr(r, k))
        self.q_cam_icrs = tuple(r.q_cam_icrs)

    def __repr__(self):
        return (f"LisResult({self.status}, ra={self.ra_deg:.5f}, dec={self.dec_deg:.5f}, roll={self.roll_deg:.5f}, "
                f"fov={self.fov_deg:.4f}, matches={self.num_matches})")


def _image(pixels, width, height, fmt):
    """Returns (pointer, keepalive, width, height, stride, format) for a 2-D image buffer."""
    shape = getattr(pixels, "shape", None)
    if shape is not None:
        height, width = int(shape[0]), int(shape[1])
        itemsize = pixels.dtype.itemsize
        if itemsize not in (1, 2):
            raise TypeError("image must be uint8 or uint16")
        fmt = MONO8 if itemsize == 1 else MONO16
        if not pixels.flags["C_CONTIGUOUS"]:
            pixels = pixels.copy(order="C")
        return ctypes.c_void_p(pixels.ctypes.data), pixels, width, height, 0, fmt
    if width is None or height is None:
        raise TypeError("width and height are required for a raw buffer")
    mv = memoryview(pixels).cast("B")
    if fmt is None:
        fmt = MONO8 if mv.nbytes == width * height else MONO16
    buf = (ctypes.c_char * mv.nbytes).from_buffer_copy(mv) if mv.readonly else (ctypes.c_char * mv.nbytes).from_buffer(mv)
    return ctypes.cast(buf, ctypes.c_void_p), buf, width, height, 0, fmt


class Tracker:
    """Database + centroider + lost-in-space solver + tracker, in memory owned by this object."""

    def __init__(self, database, camera, max_centroids=64, roi_tracking=True, centroid_sigma=2.0,
                 match_radius_px=4.0, min_matches=5, variant="esa"):
        """variant: lost-in-space search, "esa" (tetra3) or "cedar" (cedar-solve, for cedar databases)."""
        self._db = (ctypes.c_char * len(database)).from_buffer_copy(database)  # stable, 8-byte aligned
        cfg = _Config()
        library.st_config_init(ctypes.byref(cfg))
        cfg.camera = camera._c()
        cfg.max_centroids = max_centroids
        cfg.roi_tracking = int(bool(roi_tracking))
        cfg.centroid_sigma = centroid_sigma
        cfg.match_radius_px = match_radius_px
        cfg.min_matches = min_matches
        cfg.variant = _variant(variant)
        self.variant = variant
        self.camera = camera
        self.max_centroids = max_centroids
        need = library.st_tracker_memory_size(ctypes.byref(cfg), self._db, len(database))
        if need == 0:
            raise StarTrackerError(5, "database")
        self._raw = ctypes.create_string_buffer(need + 64)
        base = (ctypes.addressof(self._raw) + 63) // 64 * 64
        h = _P()
        s = library.st_tracker_create(ctypes.byref(cfg), self._db, len(database), base, need, ctypes.byref(h))
        if s != 0:
            raise StarTrackerError(s, "st_tracker_create")
        self._h = h

    def track_image(self, pixels, t_ns=0, width=None, height=None, fmt=None):
        p, keep, w, h, stride, f = _image(pixels, width, height, fmt)
        out = _Solution()
        library.st_track_image(self._h, int(t_ns), p, w, h, stride, f, ctypes.byref(out))
        del keep
        return Solution(out)

    def extract_centroids(self, pixels, width=None, height=None, fmt=None):
        """[(x, y), ...] brightest first."""
        p, keep, w, h, stride, f = _image(pixels, width, height, fmt)
        arr = (_Centroid * self.max_centroids)()
        n = ctypes.c_uint32()
        s = library.st_extract_centroids(self._h, p, w, h, stride, f, arr, self.max_centroids, ctypes.byref(n))
        del keep
        if s != 0:
            raise StarTrackerError(s, "st_extract_centroids")
        return [(arr[i].x, arr[i].y) for i in range(n.value)]

    @staticmethod
    def _cents(centroids):
        arr = (_Centroid * len(centroids))()
        for i, (x, y) in enumerate(centroids):
            arr[i].x, arr[i].y = x, y
        return arr

    def track_centroids(self, centroids, t_ns=0):
        out = _Solution()
        library.st_track_centroids(self._h, int(t_ns), self._cents(centroids), len(centroids), ctypes.byref(out))
        return Solution(out)

    def solve_lost_in_space(self, centroids, width, height, fov_estimate_deg=0.0, fov_max_error_deg=0.0,
                            pattern_checking_stars=0, match_radius=0.0, match_threshold=0.0, distortion=0.0,
                            variant=None):
        """variant: "esa" or "cedar"; None uses the tracker's."""
        o = _LisOptions(fov_estimate_deg, fov_max_error_deg, pattern_checking_stars, match_radius,
                        match_threshold, distortion, _variant(self.variant if variant is None else variant))
        r = _LisResult()
        library.st_solve_lost_in_space(self._h, self._cents(centroids), len(centroids), width, height,
                                       ctypes.byref(o), ctypes.byref(r))
        return LisResult(r)

    def set_attitude(self, q_cam_icrs, t_ns=0):
        library.st_tracker_set_attitude(self._h, int(t_ns), (ctypes.c_double * 4)(*q_cam_icrs))

    def reset(self):
        library.st_tracker_reset(self._h)
