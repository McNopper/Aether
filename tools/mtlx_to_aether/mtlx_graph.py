"""Scene-independent MaterialX node-graph resolver and texture baker.

Aether stores OpenPBR Surface parameters and *textures*; it never executes
MaterialX. This module resolves every connected OpenPBR surface input through
the document's node graph and, where the result is not a plain image, bakes it
into an equivalent Aether texture.

Supported nodes (MaterialX 1.39 semantics):

* ``image``          ``file`` + ``texcoord`` (``st`` / ``st1`` via ``geompropvalue``), UDIM
                     file sets, per-type channel read (``float`` reads ``r``), sRGB decode for
                     color-typed outputs only
* ``constant``       its ``value``
* ``extract``        ``in[index]`` (index default 0)
* ``invert``         ``amount - in``
* ``combine3``       ``vec3(in1, in2, in3)``
* ``remap``          ``outlow + (in - inlow) * (outhigh - outlow) / (inhigh - inlow)``
* ``colorcorrect``   exposure, contrast, saturation, hue, gamma, lift, gain (see ``_op_colorcorrect``)
* ``ramp4``          bilinear blend of the four corner values over the texture coordinates
* ``heighttonormal`` tangent-space normal from texel differences of a height field
* ``normalmap``      ``in * 2 - 1`` on all channels, xy scaled (kept in tangent space)

Color images are decoded from their declared color space before any arithmetic
(MaterialX semantics); data images are read raw. Outputs are encoded per slot:
color maps are written sRGB-encoded, data maps (ORM, normal, opacity) are raw.

Pixel values are ``float32`` in ``[0, 1]`` (HDR may exceed 1); every texture is
``(H, W, C)``. Nothing here knows about a scene, an asset library or a
directory layout. No third-party module is imported until a graph is actually
baked; resolving without a ``TextureStore`` needs the standard library only.
"""

from __future__ import annotations

import hashlib
import os
import re
import struct
import xml.etree.ElementTree as ET
import zlib
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

UDIM_MARKER = "<UDIM>"

# A raw ``<UDIM>`` token in an XML attribute value makes a document invalid XML.
_RAW_UDIM = re.compile(rb"<\s*(UDIM|udim)\s*>")

CHANNEL_COUNT = {
    "float": 1,
    "color3": 3,
    "color4": 4,
    "vector2": 2,
    "vector3": 3,
    "vector4": 4,
}

# Rec. 709 luma of the implicit MaterialX ``color3 -> float`` conversion.
LUMA_REC709 = (0.2126, 0.7152, 0.0722)

# ``colorcorrect`` applies a gamma *correction*, ``in^(1/gamma)``. MaterialX 1.39.5 ships the
# nodedef (``ND_colorcorrect_color3``) but no public reference implementation; set this to False
# to use the direct exponent ``in^gamma`` instead.
COLORCORRECT_GAMMA_RECIPROCAL = True

# MaterialX keeps a node's inputs in declaration order: ``getInput(name)`` returns the first input
# of that name, while its validator reports "Duplicate child name found" for the document. The
# converter therefore defaults to the runtime behavior (first wins) and reports every duplicate;
# ``last`` and ``error`` select the other two readings.
DUPLICATE_POLICIES = ("first", "last", "error")


# ── errors ──────────────────────────────────────────────────────────────────


class MaterialXError(RuntimeError):
    """Base error for the converter."""


class MaterialXParseError(MaterialXError):
    """The .mtlx file could not be parsed as XML."""


class MaterialXGraphError(MaterialXError):
    """A node, input, color space or file the resolver cannot evaluate."""


class MaterialXUdimError(MaterialXError):
    """A UDIM file set could not be resolved."""


class _BakeRequired(Exception):
    """Internal: an operator received an image that was not loaded (symbolic mode)."""

    def __init__(self, node: Node | None) -> None:
        super().__init__(node.path() if node else "graph")
        self.node = node


# ── small helpers ───────────────────────────────────────────────────────────


def _strip_ns(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def _numpy():
    """Import numpy on first use (a symbolic conversion never needs it)."""
    try:
        import numpy
    except ImportError as exc:  # pragma: no cover - environment specific
        raise MaterialXGraphError(
            "baking a node graph needs numpy: " + str(exc)
        ) from exc
    return numpy


def _pil():
    try:
        from PIL import Image
    except ImportError as exc:  # pragma: no cover - environment specific
        raise MaterialXGraphError(
            "baking a node graph needs Pillow: " + str(exc)
        ) from exc
    Image.MAX_IMAGE_PIXELS = None
    return Image


def classify_colorspace(token: str | None) -> str:
    """Classify a MaterialX file color space as ``"srgb"`` (decode) or ``"linear"`` (raw)."""
    text = (token or "").strip().lower()
    if not text or text in {
        "raw",
        "none",
        "data",
        "maskz",
        "linear",
        "lin_rec709",
        "lin_rec2020",
    }:
        return "linear"
    if "srgb" in text:
        return "srgb"
    raise MaterialXGraphError(f"unsupported image color space '{token}'")


def map_aether_colorspace(token: str | None, fallback: str) -> str:
    """Map a MaterialX color space token to an Aether texture color space token."""
    if not token:
        return fallback
    text = token.strip().lower()
    if "2020" in text:
        return "lin_rec2020_scene"
    if "srgb" in text:
        return "srgb_rec709_scene"
    if "lin_rec709" in text or "709" in text:
        return "lin_rec709_scene"
    if text in {"raw", "none", "data"}:
        return "data"
    return fallback


# ── document loading and UDIM normalization ────────────────────────────────


@dataclass
class NormalizedDocument:
    """A document whose raw ``<UDIM>`` tokens were escaped into a copy outside the source tree."""

    source: Path
    normalized: Path
    escaped_tokens: int


def _escape_udim(data: bytes) -> bytes:
    return _RAW_UDIM.sub(rb"&lt;\1&gt;", data)


def load_xml(
    source: Path, *, normalize_udim: bool = False, normalized_dir: Path | None = None
) -> tuple[ET.Element, NormalizedDocument | None]:
    """Parse a MaterialX document.

    By default malformed XML is rejected. With ``normalize_udim`` only raw ``<UDIM>`` tokens are
    escaped (``&lt;UDIM&gt;``) in a copy written to ``normalized_dir``, which must lie outside the
    source tree; the original is never touched and anything else malformed still fails.
    """
    source = Path(source)
    data = source.read_bytes()
    try:
        return ET.fromstring(data), None
    except ET.ParseError as error:
        if not normalize_udim:
            raise MaterialXParseError(
                f"Failed to parse MaterialX XML: {error}"
            ) from error
        first_error = error

    tokens = len(_RAW_UDIM.findall(data))
    if tokens == 0:
        raise MaterialXParseError(
            f"Failed to parse MaterialX XML (no raw <UDIM> token to escape): {first_error}"
        ) from first_error
    try:
        root = ET.fromstring(_escape_udim(data))
    except ET.ParseError as error:
        raise MaterialXParseError(
            f"Document stays malformed after escaping {tokens} raw <UDIM> token(s): {error}"
        ) from first_error

    if normalized_dir is None:
        raise MaterialXError("normalize_udim needs normalized_dir for the escaped copy")
    target = Path(normalized_dir) / source.name
    if target.resolve().is_relative_to(source.parent.resolve()):
        raise MaterialXUdimError(
            f"normalized copies must be written outside the source tree: {target} is inside "
            f"{source.parent}"
        )
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(_escape_udim(data))
    return root, NormalizedDocument(
        source=source, normalized=target, escaped_tokens=tokens
    )


# ── node graph model ────────────────────────────────────────────────────────


@dataclass
class DuplicateInput:
    """An element that declares the same input name more than once."""

    node: str
    input: str
    declarations: int
    resolution: str
    identical: bool

    def message(self) -> str:
        state = "identical" if self.identical else "different"
        return (
            f"duplicate input '{self.input}' on {self.node}: {self.declarations} declarations, "
            f"{state}, using the {self.resolution}"
        )


@dataclass
class Node:
    name: str
    category: str
    node_type: str
    inputs: dict[str, ET.Element]
    element: ET.Element

    def path(self) -> str:
        return f"{self.category} '{self.name}'"


def connection_of(inp: ET.Element | None) -> str | None:
    """Return the node name an input is connected to, or None for a literal input."""
    if inp is None:
        return None
    return inp.get("nodename") or inp.get("nodegraph") or None


def _declaration_key(inp: ET.Element) -> str:
    return ";".join(f"{key}={value}" for key, value in sorted(inp.attrib.items()))


class Document:
    """Every named node of a document (root and ``nodegraph`` scopes), indexed by name."""

    def __init__(self, root: ET.Element, *, duplicate_policy: str = "first") -> None:
        if duplicate_policy not in DUPLICATE_POLICIES:
            raise ValueError(f"duplicate_policy must be one of {DUPLICATE_POLICIES}")
        self.duplicate_policy = duplicate_policy
        self.nodes: dict[str, Node] = {}
        self.duplicates: list[DuplicateInput] = []
        for container in root.iter():
            if _strip_ns(container.tag) not in {"materialx", "nodegraph"}:
                continue
            for element in container:
                name = element.get("name")
                if name and name not in self.nodes:
                    self.nodes[name] = self.make_node(element)

    def make_node(self, element: ET.Element) -> Node:
        """Build a node view; inputs of one name are resolved with the duplicate policy."""
        category = _strip_ns(element.tag)
        name = element.get("name") or ""
        declared: dict[str, list[ET.Element]] = {}
        for child in element:
            input_name = child.get("name") if _strip_ns(child.tag) == "input" else None
            if input_name:
                declared.setdefault(input_name, []).append(child)
        inputs: dict[str, ET.Element] = {}
        for input_name, elements in declared.items():
            inputs[input_name] = elements[0]
            if len(elements) < 2:
                continue
            record = DuplicateInput(
                node=f"{category} '{name}'",
                input=input_name,
                declarations=len(elements),
                resolution=self.duplicate_policy,
                identical=len({_declaration_key(e) for e in elements}) == 1,
            )
            if self.duplicate_policy == "error":
                raise MaterialXGraphError(
                    f"Duplicate child name found: {input_name} (on {category} '{name}')"
                )
            self.duplicates.append(record)
            if self.duplicate_policy == "last":
                inputs[input_name] = elements[-1]
        node_type = element.get("type") or ""
        return Node(
            name=name,
            category=category,
            node_type=node_type,
            inputs=inputs,
            element=element,
        )

    def node(self, name: str) -> Node:
        node = self.nodes.get(name)
        if node is None:
            raise MaterialXGraphError(f"unknown node '{name}'")
        return node


# ── signals ─────────────────────────────────────────────────────────────────


@dataclass(frozen=True)
class DirectImage:
    """An image node that reaches a surface input untouched (referenced, not baked)."""

    path: Path
    file_value: str
    aether_colorspace: str
    is_color: bool
    channels: int
    uv: str


@dataclass
class Signal:
    """A resolved value: a per-channel constant, a texture ``(H, W, C)``, or an unbaked image."""

    channels: tuple[float, ...] | None = None
    array: object | None = None
    is_color: bool = False
    direct: DirectImage | None = None
    node: Node | None = None

    @property
    def is_texture(self) -> bool:
        return self.array is not None

    @property
    def is_constant(self) -> bool:
        return self.array is None and self.direct is None


def constant(*values: float, is_color: bool = False) -> Signal:
    return Signal(channels=tuple(float(v) for v in values), is_color=is_color)


def channels_of(signal: Signal) -> int:
    if signal.array is not None:
        return int(signal.array.shape[2])
    if signal.channels is not None:
        return len(signal.channels)
    if signal.direct is not None:
        return signal.direct.channels
    raise MaterialXGraphError("value has no channels")


def convert(signal: Signal, count: int) -> Signal:
    """Apply the implicit MaterialX conversion of ``signal`` to ``count`` channels.

    ``color3 -> float`` is the Rec. 709 luminance, ``vectorN -> float`` the first component, a
    scalar is replicated, a shorter value is padded (alpha 1, else 0) and a longer one truncated.
    """
    np = _numpy()
    array = signal.array
    if array is None:
        if signal.channels is None:
            raise _BakeRequired(signal.node)
        values = list(signal.channels)
        if len(values) == count:
            return signal
        if count == 1:
            if len(values) == 1:
                return constant(values[0])
            if signal.is_color and len(values) >= 3:
                return constant(sum(v * w for v, w in zip(values, LUMA_REC709)))
            return constant(values[0])
        if len(values) == 1:
            values = values * count
        elif len(values) < count:
            values = values + [1.0 if signal.is_color else 0.0] * (count - len(values))
        return constant(*values[:count], is_color=signal.is_color)

    have = int(array.shape[2])
    if have == count:
        return signal
    if count == 1:
        if have == 1:
            return Signal(array=np.ascontiguousarray(array[..., :1]), node=signal.node)
        if signal.is_color and have >= 3:
            red, green, blue = (array[..., i : i + 1] for i in range(3))
            return Signal(
                array=red * LUMA_REC709[0]
                + green * LUMA_REC709[1]
                + blue * LUMA_REC709[2],
                node=signal.node,
            )
        return Signal(array=np.ascontiguousarray(array[..., :1]), node=signal.node)
    if have == 1:
        return Signal(
            array=np.repeat(array, count, axis=2),
            is_color=signal.is_color,
            node=signal.node,
        )
    if have < count:
        fill = np.full(
            (*array.shape[:2], count - have),
            1.0 if signal.is_color else 0.0,
            dtype=np.float32,
        )
        return Signal(
            array=np.concatenate([array, fill], axis=2),
            is_color=signal.is_color,
            node=signal.node,
        )
    return Signal(
        array=np.ascontiguousarray(array[..., :count]),
        is_color=signal.is_color,
        node=signal.node,
    )


def srgb_decode(array):
    """MaterialX ``mx_srgb_texture_to_lin_rec709`` (the texture variant, threshold 0.04045)."""
    np = _numpy()
    lower = array / 12.92
    higher = np.power((array + 0.055) / 1.055, 2.4)
    return np.where(array < 0.04045, lower, higher).astype(np.float32)


def srgb_encode(array):
    np = _numpy()
    lower = array * 12.92
    higher = np.power(np.maximum(array, 0.0), 1.0 / 2.4) * 1.055 - 0.055
    return np.where(array <= 0.0031308, lower, higher).astype(np.float32)


# ── UDIM file sets ──────────────────────────────────────────────────────────


@dataclass(frozen=True)
class UdimPattern:
    prefix: str
    suffix: str

    def files(self, directory: Path) -> dict[int, Path]:
        """Map tile id -> file for every ``<prefix><tile><suffix>`` file in ``directory``."""
        tiles: dict[int, Path] = {}
        if not directory.is_dir():
            return tiles
        for entry in os.scandir(directory):
            name = entry.name
            if name.startswith(self.prefix) and name.endswith(self.suffix):
                middle = name[len(self.prefix) : len(name) - len(self.suffix)]
                if middle.isdigit():
                    tiles[int(middle)] = directory / name
        return tiles


def split_udim(file_value: str) -> UdimPattern | None:
    """Return the ``<UDIM>`` pattern of a file value, or None when it has no UDIM token."""
    if UDIM_MARKER not in file_value:
        if any(token in file_value for token in ("<UVTILE>", "<FRAME>", "*")):
            raise MaterialXUdimError(
                f"unsupported filename substitution in '{file_value}'"
            )
        return None
    if file_value.count(UDIM_MARKER) != 1:
        raise MaterialXUdimError(f"more than one <UDIM> token in '{file_value}'")
    prefix, suffix = file_value.split(UDIM_MARKER)
    return UdimPattern(prefix=prefix, suffix=suffix)


# ── operator implementations ────────────────────────────────────────────────


def _parse_literal(inp: ET.Element) -> Signal:
    text = (inp.get("value") or "").strip()
    kind = (inp.get("type") or "float").lower()
    if kind == "boolean":
        return constant(1.0 if text.lower() in {"true", "1", "yes"} else 0.0)
    parts = [p for p in text.replace(",", " ").split() if p]
    try:
        return constant(*(float(p) for p in parts), is_color=kind.startswith("color"))
    except ValueError as exc:
        raise MaterialXGraphError(
            f"cannot parse input '{inp.get('name')}' value '{text}'"
        ) from exc


def _run(
    ev: Evaluator, node: Node, signals: list[Signal], fn: Callable[..., object]
) -> Signal:
    """Evaluate ``fn`` over resolved signals: on constants, or per texel on one shared shape."""
    np = _numpy()
    is_color = any(s.is_color for s in signals)
    if all(s.is_constant for s in signals):
        out = fn(*[np.asarray(s.channels, dtype=np.float64) for s in signals])
        return constant(*[float(v) for v in np.ravel(out)], is_color=is_color)
    shape = next((s.array.shape[:2] for s in signals if s.array is not None), None)
    if shape is None:
        raise _BakeRequired(node)
    array = fn(*[ev.broadcast(s, shape) for s in signals])
    return Signal(
        array=np.ascontiguousarray(array, dtype=np.float32),
        is_color=is_color,
        node=node,
    )


def _op_constant(ev: Evaluator, node: Node) -> Signal:
    signal = ev.value(node, "value") or constant(0.0)
    return Signal(channels=signal.channels, is_color=node.node_type.startswith("color"))


def _op_extract(ev: Evaluator, node: Node) -> Signal:
    source = ev.value(node, "in")
    if source is None:
        raise MaterialXGraphError(f"{node.path()}: input 'in' is required")
    index = int(ev.scalar(node, "index", 0.0))
    if source.is_constant:
        channels = source.channels or ()
        if index >= len(channels):
            raise MaterialXGraphError(f"{node.path()}: index {index} out of range")
        return constant(channels[index])
    if source.array is None:
        raise _BakeRequired(source.node or node)
    if index >= source.array.shape[2]:
        raise MaterialXGraphError(
            f"{node.path()}: index {index} out of range for {source.array.shape[2]} channels"
        )
    np = _numpy()
    return Signal(
        array=np.ascontiguousarray(source.array[..., index : index + 1]), node=node
    )


def _op_invert(ev: Evaluator, node: Node) -> Signal:
    source = ev.value(node, "in")
    if source is None:
        raise MaterialXGraphError(f"{node.path()}: input 'in' is required")
    count = channels_of(source)
    amount = convert(ev.value(node, "amount", 1, 1.0) or constant(1.0), count)
    return _run(ev, node, [amount, convert(source, count)], lambda a, x: a - x)


def _op_combine3(ev: Evaluator, node: Node) -> Signal:
    np = _numpy()
    signals = []
    for name in ("in1", "in2", "in3"):
        signal = ev.value(node, name, 1)
        if signal is None:
            raise MaterialXGraphError(f"{node.path()}: input '{name}' is required")
        signals.append(signal)
    return _run(ev, node, signals, lambda *a: np.concatenate(a, axis=-1))


def _op_remap(ev: Evaluator, node: Node) -> Signal:
    source = ev.value(node, "in")
    if source is None:
        raise MaterialXGraphError(f"{node.path()}: input 'in' is required")
    count = channels_of(source)
    defaults = {"inlow": 0.0, "inhigh": 1.0, "outlow": 0.0, "outhigh": 1.0}
    signals = [convert(source, count)]
    for name, default in defaults.items():
        signals.append(
            convert(ev.value(node, name, None, default) or constant(default), count)
        )

    def remap(x, inlow, inhigh, outlow, outhigh):
        np = _numpy()
        if np.any(inhigh == inlow):
            raise MaterialXGraphError(
                f"{node.path()}: 'inhigh' must differ from 'inlow'"
            )
        return outlow + (x - inlow) * (outhigh - outlow) / (inhigh - inlow)

    return _run(ev, node, signals, remap)


def _hue_rotate(rgb, hue):
    """MaterialX ``hsvadjust``: add ``hue`` (wrapping at 0-1) through an RGB -> HSV -> RGB round trip."""
    np = _numpy()
    red, green, blue = (rgb[..., i : i + 1] for i in range(3))
    top = np.maximum(np.maximum(red, green), blue)
    bottom = np.minimum(np.minimum(red, green), blue)
    delta = top - bottom
    safe = np.where(delta > 0.0, delta, 1.0)
    sector = np.where(
        top == red,
        ((green - blue) / safe) % 6.0,
        np.where(top == green, (blue - red) / safe + 2.0, (red - green) / safe + 4.0),
    )
    h = np.where(delta > 0.0, sector / 6.0, 0.0)
    s = np.where(top > 0.0, delta / np.where(top > 0.0, top, 1.0), 0.0)
    h = (h + hue) % 1.0
    value = top
    index = np.floor(h * 6.0)
    f = h * 6.0 - index
    p = value * (1.0 - s)
    q = value * (1.0 - s * f)
    t = value * (1.0 - s * (1.0 - f))
    which = (
        index.astype(np.int64) % 6
    )  # keeps the singleton channel axis for broadcasting
    tables = (
        (value, t, p),
        (q, value, p),
        (p, value, t),
        (p, q, value),
        (t, p, value),
        (value, p, q),
    )
    channels = [np.zeros_like(value) for _ in range(3)]
    for sector, table in enumerate(tables):
        mask = which == sector
        for channel in range(3):
            channels[channel] = np.where(mask, table[channel], channels[channel])
    return np.concatenate(channels, axis=-1)


def _colorcorrect(
    x, hue, saturation, gamma, lift, gain, contrast, pivot, exposure, node: Node
):
    """MaterialX ``colorcorrect``: the adjustment nodes it combines, applied in port order.

    exposure (``2^exposure``), contrast (linear around ``contrastpivot``), saturation (``saturate``),
    hue (``hsvadjust``), gamma, then lift and gain. Each port is documented on
    ``ND_colorcorrect_color3``; the order and the gamma convention are stated in the module doc.
    """
    np = _numpy()
    x = x * np.power(2.0, exposure)
    x = (x - pivot) * contrast + pivot
    if np.any(saturation != 1.0):
        luma = (
            x[..., 0:1] * LUMA_REC709[0]
            + x[..., 1:2] * LUMA_REC709[1]
            + x[..., 2:3] * LUMA_REC709[2]
        )
        x = luma + saturation * (x - luma)
    if np.any(hue != 0.0):
        x = _hue_rotate(
            x, hue[..., 0:1]
        )  # one rotation for the color, not one per channel
    if np.any(gamma == 0.0):
        raise MaterialXGraphError(f"{node.path()}: 'gamma' must not be 0")
    exponent = 1.0 / gamma if COLORCORRECT_GAMMA_RECIPROCAL else gamma
    x = np.sign(x) * np.power(np.abs(x), exponent)
    x = x + lift * (1.0 - x)
    return x * gain


def _op_colorcorrect(ev: Evaluator, node: Node) -> Signal:
    source = ev.value(node, "in", 3)
    if source is None:
        raise MaterialXGraphError(f"{node.path()}: input 'in' is required")
    defaults = {
        "hue": 0.0,
        "saturation": 1.0,
        "gamma": 1.0,
        "lift": 0.0,
        "gain": 1.0,
        "contrast": 1.0,
        "contrastpivot": 0.5,
        "exposure": 0.0,
    }
    signals = [source]
    for name, default in defaults.items():
        signals.append(
            convert(ev.value(node, name, None, default) or constant(default), 3)
        )
    return _run(
        ev,
        node,
        signals,
        lambda *s: _colorcorrect(*s, node),
    )


def _op_ramp4(ev: Evaluator, node: Node) -> Signal:
    corners = {}
    for name in ("valuetl", "valuetr", "valuebl", "valuebr"):
        signal = ev.value(node, name, 1, 0.0) or constant(0.0)
        if not signal.is_constant:
            raise MaterialXGraphError(f"{node.path()}: corner values must be constants")
        corners[name] = signal.channels[0]
    ev.uv_set(node)
    if ev.shape is None:
        raise MaterialXGraphError(
            f"{node.path()}: no image in the graph defines the texture resolution (bake it first)"
        )
    np = _numpy()
    height, width = ev.shape
    u = ((np.arange(width, dtype=np.float32) + 0.5) / width)[None, :, None]
    v = ((np.arange(height, dtype=np.float32) + 0.5) / height)[:, None, None]
    bottom = corners["valuebl"] + (corners["valuebr"] - corners["valuebl"]) * u
    top = corners["valuetl"] + (corners["valuetr"] - corners["valuetl"]) * u
    return Signal(array=(bottom + (top - bottom) * v).astype(np.float32), node=node)


def _op_heighttonormal(ev: Evaluator, node: Node) -> Signal:
    source = ev.value(node, "in", 1)
    if source is None:
        raise MaterialXGraphError(f"{node.path()}: input 'in' is required")
    scale = ev.scalar(node, "scale", 1.0)
    ev.uv_set(node)
    ev.approximations.append(
        f"{node.path()}: heighttonormal is baked with texel-space central differences (periodic "
        "wrap); MaterialX takes screen-space derivatives, so the result is view independent here"
    )
    if source.is_constant:
        return constant(0.5, 0.5, 1.0)
    if source.array is None:
        raise _BakeRequired(source.node or node)
    np = _numpy()
    heights = source.array[..., 0]
    height, width = heights.shape
    strength = (
        scale / 16.0
    )  # MaterialX: SOBEL_SCALE_FACTOR, height measured in texcoord units
    du = (np.roll(heights, -1, axis=1) - np.roll(heights, 1, axis=1)) * (width * 0.5)
    dv = (np.roll(heights, -1, axis=0) - np.roll(heights, 1, axis=0)) * (height * 0.5)
    nx = -strength * du
    ny = -strength * dv
    nz = np.ones_like(nx)
    normal = np.stack((nx, ny, nz), axis=2)
    normal /= np.sqrt(np.sum(normal * normal, axis=2, keepdims=True))
    return Signal(array=(normal * 0.5 + 0.5).astype(np.float32), node=node)


def _op_normalmap(ev: Evaluator, node: Node) -> Signal:
    source = ev.value(node, "in", 3)
    if source is None:
        raise MaterialXGraphError(f"{node.path()}: input 'in' is required")
    for name in ("normal", "tangent", "bitangent"):
        if connection_of(node.inputs.get(name)):
            raise MaterialXGraphError(
                f"{node.path()}: a connected '{name}' input (world-space frame) cannot be baked"
            )
    scale = ev.value(node, "scale", None, 1.0) or constant(1.0)
    count = channels_of(scale)
    if count not in (1, 2):
        raise MaterialXGraphError(
            f"{node.path()}: 'scale' must be a float or a vector2"
        )
    values = scale.channels if scale.is_constant else None
    if values is None:
        raise MaterialXGraphError(f"{node.path()}: 'scale' must be a constant")
    sx, sy = (values[0], values[0]) if count == 1 else (values[0], values[1])
    if source.is_constant:
        r, g, b = source.channels or (0.0, 0.0, 1.0)
        if (r, g, b) == (0.0, 0.0, 0.0):
            return constant(0.0, 0.0, 1.0)
        return constant((2.0 * r - 1.0) * sx, (2.0 * g - 1.0) * sy, 2.0 * b - 1.0)
    if source.array is None:
        raise _BakeRequired(source.node or node)
    np = _numpy()
    vector = source.array * 2.0 - 1.0
    flat = (vector == -1.0).all(axis=2, keepdims=True)
    vector = np.where(flat, np.float32([0.0, 0.0, 1.0]), vector)
    vector[..., 0] *= sx
    vector[..., 1] *= sy
    return Signal(array=vector.astype(np.float32), node=node)


def _op_image(ev: Evaluator, node: Node) -> Signal:
    file_input = node.inputs.get("file")
    if file_input is None or connection_of(file_input):
        raise MaterialXGraphError(f"{node.path()}: a literal 'file' input is required")
    file_value = file_input.get("value") or ""
    if not file_value:
        raise MaterialXGraphError(f"{node.path()}: the 'file' input is empty")
    for name in ("layer", "framerange"):
        other = node.inputs.get(name)
        if other is not None and (
            connection_of(other) or (other.get("value") or "").strip()
        ):
            raise MaterialXGraphError(
                f"{node.path()}: the '{name}' input is not supported"
            )
    uv = ev.uv_set(node)
    node_type = node.node_type or "color3"
    channels = CHANNEL_COUNT.get(node_type)
    if channels is None:
        raise MaterialXGraphError(
            f"{node.path()}: unsupported image type '{node_type}'"
        )
    is_color = node_type.startswith("color")
    token = file_input.get("colorspace") or node.element.get("colorspace")
    kind = classify_colorspace(token)
    direct = DirectImage(
        path=ev.resolve_file(node, file_value),
        file_value=file_value,
        aether_colorspace=map_aether_colorspace(
            token, "srgb_rec709_scene" if is_color else "data"
        ),
        is_color=is_color,
        channels=channels,
        uv=uv,
    )
    ev.source_files.add(direct.path)
    if ev.store is None:
        return Signal(direct=direct, is_color=is_color, node=node)
    array = convert(
        Signal(array=ev.store.load(direct.path, node), is_color=is_color), channels
    ).array
    if is_color and kind == "srgb":
        array = srgb_decode(array)
    ev.shape = (int(array.shape[0]), int(array.shape[1]))
    # `direct` stays set: a slot may reference the untouched file instead of a baked copy.
    return Signal(array=array, is_color=is_color, direct=direct, node=node)


_HANDLERS: dict[str, Callable[[Evaluator, Node], Signal]] = {
    "constant": _op_constant,
    "image": _op_image,
    "extract": _op_extract,
    "invert": _op_invert,
    "combine3": _op_combine3,
    "remap": _op_remap,
    "colorcorrect": _op_colorcorrect,
    "ramp4": _op_ramp4,
    "heighttonormal": _op_heighttonormal,
    "normalmap": _op_normalmap,
}

SUPPORTED_NODE_CATEGORIES = tuple(sorted(_HANDLERS))


# ── evaluator ───────────────────────────────────────────────────────────────


class Evaluator:
    """Evaluates the nodes of one document for one material variant (one UDIM tile)."""

    def __init__(
        self,
        doc: Document,
        *,
        source_dir: Path,
        store: TextureStore | None = None,
        tile_files: dict[str, Path] | None = None,
    ) -> None:
        self.doc = doc
        self.source_dir = Path(source_dir)
        self.store = store
        self.tile_files = dict(tile_files or {})
        self._memo: dict[int, Signal] = {}
        self._active: set[int] = set()
        self.shape: tuple[int, int] | None = None
        self.source_files: set[Path] = set()
        self.uv_sets: set[str] = set()
        self.approximations: list[str] = []

    def evaluate(self, node: Node) -> Signal:
        key = id(node.element)
        if key in self._memo:
            return self._memo[key]
        handler = _HANDLERS.get(node.category)
        if handler is None:
            raise MaterialXGraphError(
                f"unsupported node {node.path()} (category '{node.category}')"
            )
        if key in self._active:
            raise MaterialXGraphError(f"cycle in the node graph at {node.path()}")
        self._active.add(key)
        try:
            signal = handler(self, node)
        except _BakeRequired as required:
            where = required.node.path() if required.node else node.path()
            raise MaterialXGraphError(
                f"{where} needs baking (a derived texture); pass texture_dir to bake it"
            ) from required
        finally:
            self._active.discard(key)
        self._memo[key] = signal
        return signal

    def evaluate_connection(self, inp: ET.Element) -> Signal:
        if inp.get("output"):
            raise MaterialXGraphError(
                f"connections to a named output ('{inp.get('output')}') are not supported"
            )
        return self.evaluate(self.doc.node(connection_of(inp) or ""))

    def input(self, node: Node, name: str) -> Signal | None:
        """The resolved signal of one input: the connected node's output, a literal, or None."""
        inp = node.inputs.get(name)
        if inp is None:
            return None
        if connection_of(inp):
            return self.evaluate_connection(inp)
        if inp.get("value") is None:
            return None
        return _parse_literal(inp)

    def value(
        self,
        node: Node,
        name: str,
        count: int | None = None,
        default: float | None = None,
    ):
        """Like ``input`` but with a default and converted to ``count`` channels."""
        signal = self.input(node, name)
        if signal is None:
            if default is None:
                return None
            signal = constant(default)
        if count is not None:
            signal = convert(signal, count)
        return signal

    def scalar(self, node: Node, name: str, default: float) -> float:
        signal = self.value(node, name, 1, default)
        if signal is None or signal.channels is None:
            raise MaterialXGraphError(
                f"{node.path()}: input '{name}' must be a constant"
            )
        return signal.channels[0]

    def uv_set(self, node: Node) -> str:
        """The geometry property a ``texcoord`` input reads (``st`` when unconnected)."""
        inp = node.inputs.get("texcoord")
        if inp is None:
            return "st"
        if not connection_of(inp):
            raise MaterialXGraphError(
                f"{node.path()}: a literal 'texcoord' is not supported"
            )
        target = self.doc.node(connection_of(inp) or "")
        if target.category != "geompropvalue" or target.node_type not in (
            "",
            "vector2",
        ):
            raise MaterialXGraphError(
                f"unsupported texcoord source {target.path()} (category '{target.category}')"
            )
        geomprop = target.inputs.get("geomprop")
        name = (geomprop.get("value") if geomprop is not None else "") or "st"
        name = {"UV0": "st", "uv0": "st", "st0": "st"}.get(name, name)
        self.uv_sets.add(name)
        return name

    def resolve_file(self, node: Node, file_value: str) -> Path:
        if split_udim(file_value) is None:
            return self.source_dir / file_value
        path = self.tile_files.get(file_value)
        if path is None:
            raise MaterialXUdimError(
                f"{node.path()}: no tile of '{file_value}' belongs to this material variant"
            )
        return path

    def broadcast(self, signal: Signal, shape: tuple[int, int]):
        """The texture of ``signal`` on ``shape``; a constant becomes a uniform texture."""
        np = _numpy()
        if signal.array is not None:
            if tuple(int(n) for n in signal.array.shape[:2]) != tuple(shape):
                raise MaterialXGraphError(
                    "textures of different sizes cannot be combined: "
                    f"{tuple(int(n) for n in signal.array.shape[:2])} and {tuple(shape)}"
                )
            return signal.array
        return np.full(
            (*shape, len(signal.channels or (0.0,))), signal.channels, dtype=np.float32
        )


# ── texture store (baking) ──────────────────────────────────────────────────


def quantize(array):
    """``float [0, 1] -> uint8``; returns the pixels and the number of texels that had to clamp."""
    np = _numpy()
    clamped = int(np.count_nonzero((array < 0.0) | (array > 1.0)))
    pixels = np.clip(np.floor(array * 255.0 + 0.5), 0.0, 255.0).astype(np.uint8)
    return pixels, clamped


def quantize16(array):
    """``float [0, 1] -> uint16`` with the same clamp accounting as :func:`quantize`."""
    np = _numpy()
    clamped = int(np.count_nonzero((array < 0.0) | (array > 1.0)))
    pixels = np.clip(np.floor(array * 65535.0 + 0.5), 0.0, 65535.0).astype(np.uint16)
    return pixels, clamped


def lies_on_8bit_grid(array) -> bool:
    """Whether every value is already exactly representable by the uint8 encoder."""
    np = _numpy()
    scaled = np.clip(array, 0.0, 1.0) * 255.0
    return bool(np.all(np.abs(scaled - np.rint(scaled)) < 1.0e-4))


def _png_chunk(kind: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + kind
        + payload
        + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
    )


def save_png16(path: Path, pixels) -> None:
    """Writes uint16 ``(H, W, C)`` without Pillow's RGB16-to-RGB8 conversion."""
    np = _numpy()
    height, width, planes = pixels.shape
    color_type = {1: 0, 2: 4, 3: 2, 4: 6}.get(planes)
    if color_type is None:
        raise MaterialXGraphError(f"cannot write {planes}-channel PNG '{path}'")
    rows = []
    for row in pixels:
        rows.append(b"\0" + np.asarray(row, dtype=">u2").tobytes())
    header = struct.pack(">IIBBBBB", width, height, 16, color_type, 0, 0, 0)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + _png_chunk(b"IHDR", header)
        + _png_chunk(b"IDAT", zlib.compress(b"".join(rows), 4))
        + _png_chunk(b"IEND", b"")
    )


def uniform_value(array):
    """The single value of a texture whose texels are all equal, else None.

    A value that lies on the grid of an 8- or 16-bit source is returned exactly (``k / 255``),
    so folding a uniform texture into a scalar reproduces the authored number and not the
    float32 rounding of the decoded texel.
    """
    np = _numpy()
    first = array[0, 0]
    if not np.all(array == first):
        return None
    values = []
    for value in (float(v) for v in first):
        snapped = None
        for levels in (255.0, 65535.0):
            grid = value * levels
            if abs(grid - round(grid)) < 1.0e-4:
                snapped = round(grid) / levels
                break
        values.append(snapped if snapped is not None else round(value, 9))
    return tuple(values)


class TextureStore:
    """Loads source textures and writes derived PNGs, deduplicated by content hash."""

    #: source textures kept decoded between evaluations (4K RGB is ~200 MB each)
    CACHE_SIZE = 3

    def __init__(self, texture_dir: Path, *, max_resolution: int | None = None) -> None:
        self.texture_dir = Path(texture_dir)
        self.max_resolution = max_resolution
        self._cache: dict[Path, object] = {}
        self._order: list[Path] = []
        self.derived: dict[str, Path] = {}  # pixel digest -> file
        self.written: set[Path] = set()

    def load(self, path: Path, node: Node | None = None):
        path = Path(path)
        if path in self._cache:
            return self._cache[path]
        where = node.path() if node else f"texture '{path}'"
        if not path.is_file():
            raise MaterialXGraphError(f"{where}: file not found: {path}")
        try:
            with _pil().open(path) as image:
                image.load()
                array = _to_array(image)
        except MaterialXGraphError:
            raise
        except Exception as exc:
            raise MaterialXGraphError(f"{where}: cannot read '{path}': {exc}") from exc
        if self.max_resolution:
            array = _reduced(array, self.max_resolution)
        self._cache[path] = array
        self._order.append(path)
        while len(self._order) > self.CACHE_SIZE:
            self._cache.pop(self._order.pop(0), None)
        return array

    def save(self, pixels, label: str) -> Path:
        """Write uint8/uint16 ``(H, W, C)`` PNG; identical pixels share one file.

        The file name starts with the digest of the pixels, so equal content from another graph,
        material or earlier run is found and reused instead of uploaded twice by the renderer.
        """
        digest = hashlib.sha256()
        if pixels.dtype != _numpy().uint8:
            # Keep the established uint8 content-addressed filenames cache-compatible;
            # wider pixels need their dtype in the key to avoid cross-precision aliasing.
            digest.update(pixels.dtype.str.encode("ascii"))
        digest.update(struct.pack("<III", *(int(n) for n in pixels.shape)))
        digest.update(pixels.tobytes())
        key = digest.hexdigest()[:16]
        if key in self.derived:
            return self.derived[key]
        existing = (
            sorted(self.texture_dir.glob(f"{key}_*.png"))
            if self.texture_dir.is_dir()
            else []
        )
        if existing:
            target = existing[0]
        else:
            self.texture_dir.mkdir(parents=True, exist_ok=True)
            target = self.texture_dir / f"{key}_{label}.png"
            planes = pixels.shape[2]
            if pixels.dtype == _numpy().uint16:
                save_png16(target, pixels)
            else:
                Image = _pil()
                Image.fromarray(pixels[..., 0] if planes == 1 else pixels).save(
                    target, format="PNG", compress_level=4
                )
            self.written.add(target)
        self.derived[key] = target
        return target


def _reduced(array, max_resolution: int):
    """Box-filter a texture by a power of two until its longer side fits ``max_resolution``."""
    np = _numpy()
    height, width = array.shape[:2]
    factor = 1
    while max(height, width) // factor > max_resolution:
        factor *= 2
    if factor == 1:
        return array
    height -= height % factor
    width -= width % factor
    blocks = array[:height, :width].reshape(
        height // factor, factor, width // factor, factor, -1
    )
    return blocks.mean(axis=(1, 3)).astype(np.float32)


def _to_array(image):
    """A loaded Pillow image as float32 ``(H, W, C)`` in ``[0, 1]`` (16-bit kept at full range)."""
    np = _numpy()
    width, height = image.size
    if image.mode.startswith("I;16"):
        dtype = ">u2" if image.mode == "I;16B" else "<u2"
        raw = np.frombuffer(image.tobytes(), dtype=dtype).astype(np.float32) / 65535.0
        return np.ascontiguousarray(raw.reshape(height, width, 1))
    if image.mode not in {"L", "LA", "RGB", "RGBA"}:
        image = image.convert("RGBA")
    return np.ascontiguousarray(
        np.asarray(image, dtype=np.float32).reshape(height, width, -1) / 255.0
    )


def encode_color(array):
    """Linear color -> (sRGB-encoded uint8 RGB, Aether color space, clamped texel count)."""
    np = _numpy()
    clamped = int(np.count_nonzero((array < 0.0) | (array > 1.0)))
    pixels, _ = quantize(srgb_encode(np.clip(array, 0.0, 1.0)))
    return pixels, "srgb_rec709_scene", clamped


def encode_normal(array):
    """Tangent-space vector in [-1, 1] -> the [0, 1] encoding Aether decodes as ``2 * tex - 1``."""
    return quantize((array + 1.0) * 0.5)[0], "data", 0


def encode_orm(roughness, metalness):
    """Aether ``map_orm``: R = 1 (no occlusion input exists in OpenPBR Surface), G, B."""
    np = _numpy()
    shape = roughness.shape[:2]
    red = np.full((*shape, 1), 1.0, dtype=np.float32)
    array = np.concatenate([red, roughness, metalness], axis=2)
    encoder = quantize if lies_on_8bit_grid(array) else quantize16
    return encoder(array)[0], "data", 0


def encode_opacity(value):
    """Aether reads the presence weight from the alpha of ``map_opacity``; keep it in every channel."""
    np = _numpy()
    return quantize(np.concatenate([value] * 4, axis=2))[0], "data", 0


def encode_scalar(value):
    """A scalar OpenPBR map in every channel, so the renderers can read ``.r`` consistently."""
    np = _numpy()
    clamped = int(np.count_nonzero((value < 0.0) | (value > 1.0)))
    array = np.concatenate([np.clip(value, 0.0, 1.0)] * 4, axis=2)
    encoder = quantize if lies_on_8bit_grid(array) else quantize16
    return encoder(array)[0], "data", clamped


def encode_emission_luminance(value):
    """Preserve unbounded HDR emission with a normalized texture and an authored scale."""
    np = _numpy()
    scale = max(1.0, float(np.max(value)))
    pixels, color_space, clamped = encode_scalar(value / scale)
    return pixels, color_space, clamped, scale


def encode_data_color(array):
    """A non-color vector map (transmission scatter): raw linear data, never sRGB-encoded."""
    np = _numpy()
    clamped = int(np.count_nonzero((array < 0.0) | (array > 1.0)))
    return quantize(np.clip(array, 0.0, 1.0))[0], "data", clamped
