# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded CAD-95 diagnosis, not a complete mesh-validity certificate.

Find a non-coplanar, positive-length crossing in each unchanged binary STL,
then certify its coordinates with Decimal arithmetic. Coplanar intersections
are deliberately outside this diagnostic's scope: absence of a witness is
not a pass. Requires NumPy. Inputs are never modified.

python tools/diagnose_fin_mesh.py /path/to/recovered/candidate/export --output report.json
"""
import argparse
from decimal import Decimal, localcontext
import hashlib
import json
from pathlib import Path
import struct

import numpy as np


def read_stl(path):
    raw = Path(path).read_bytes()
    count = struct.unpack_from('<I', raw, 80)[0]
    if len(raw) != 84 + 50 * count:
        raise ValueError('Expected an exact-size binary STL: ' + str(path))
    dtype = np.dtype([('normal', '<f4', (3,)), ('vertices', '<f4', (3, 3)), ('attribute', '<u2')])
    return np.frombuffer(raw, dtype=dtype, offset=84)['vertices'].astype(float)


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def section(triangle, distances):
    points = [v for v, d in zip(triangle, distances) if d == 0]
    for i, j in ((0, 1), (1, 2), (2, 0)):
        if distances[i] * distances[j] < 0:
            t = distances[i] / (distances[i] - distances[j])
            points.append(tuple(a + t * (b - a) for a, b in zip(triangle[i], triangle[j])))
    return points


def certify(a, b):
    """Certify intersection of two non-coplanar triangles beyond shared topology."""
    with localcontext() as ctx:
        ctx.prec = 80
        a, b = [tuple(tuple(Decimal.from_float(float(v)) for v in p) for p in t) for t in (a, b)]
        shared = set(a) & set(b)
        if len(shared) >= 2:
            return None  # A non-coplanar common edge is legal adjacency.
        na, nb = [cross(sub(t[1], t[0]), sub(t[2], t[0])) for t in (a, b)]
        direction = cross(na, nb)
        axis = max(range(3), key=lambda k: abs(direction[k]))
        if direction[axis] == 0:
            return None  # Parallel/coplanar/degenerate: deliberately not assessed.
        sa = section(a, [dot(sub(v, b[0]), nb) for v in a])
        sb = section(b, [dot(sub(v, a[0]), na) for v in b])
        if len(sa) < 2 or len(sb) < 2:
            return None
        sa.sort(key=lambda p: p[axis])
        sb.sort(key=lambda p: p[axis])
        lo = max(sa[0][axis], sb[0][axis])
        hi = min(sa[-1][axis], sb[-1][axis])
        if hi <= lo:
            return None
        def point(value):
            t = (value - sa[0][axis]) / (sa[-1][axis] - sa[0][axis])
            return tuple(x + t * (y - x) for x, y in zip(sa[0], sa[-1]))
        start, end = point(lo), point(hi)
        delta = sub(end, start)
        return {'segment_length_mm': str(dot(delta, delta).sqrt()),
                'start': [str(x) for x in start], 'end': [str(x) for x in end],
                'shared_vertices': len(shared), 'decimal_precision': ctx.prec}


def find_crossing(triangles):
    """Float broad/narrow phase, followed by Decimal confirmation; first witness only."""
    mins, maxs = triangles.min(axis=1), triangles.max(axis=1)
    normals = np.cross(triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0])
    candidates = 0
    for i, a in enumerate(triangles):
        js = np.flatnonzero(np.all(maxs[i] >= mins[i + 1:], axis=1) &
                            np.all(mins[i] <= maxs[i + 1:], axis=1)) + i + 1
        if not len(js):
            continue
        b = triangles[js]
        da = np.einsum('nkj,nj->nk', a[None, :, :] - b[:, None, 0, :], normals[js])
        db = np.einsum('nkj,j->nk', b - a[0], normals[i])
        keep = (da.min(axis=1) <= 0) & (da.max(axis=1) >= 0) & (db.min(axis=1) <= 0) & (db.max(axis=1) >= 0)
        for j in js[keep]:
            candidates += 1
            certificate = certify(a, triangles[j])
            if certificate:
                return {'triangle_indices_zero_based': [i, int(j)],
                        'triangles': [a.tolist(), triangles[j].tolist()],
                        'certificate': certificate, 'decimal_candidates_checked': candidates}
    return {'witness': None, 'interpretation': 'No witness found; NOT a validity pass.',
            'decimal_candidates_checked': candidates}


def controls():
    a = np.array([[0, 0, 0], [2, 0, 0], [0, 2, 0]], float)
    crossing = np.array([[0, 0, 0], [1, 1, -1], [1, 1, 1]], float)
    touching = np.array([[0, 0, 0], [-1, 0, 1], [0, -1, 1]], float)
    adjacent = np.array([[0, 0, 0], [2, 0, 0], [0, 0, 2]], float)
    assert certify(a, crossing) is not None
    assert certify(a, touching) is None
    assert certify(a, adjacent) is None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    controls()
    result = {'issue': 'CAD-95', 'scope': 'First confirmed non-coplanar crossing, not exhaustive validation',
              'input_files_modified': False, 'parts': []}
    for path in sorted(args.folder.glob('*.stl')):
        triangles = read_stl(path)
        result['parts'].append({'file': path.name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                                'triangle_count': len(triangles), **find_crossing(triangles)})
    rendered = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.write_text(rendered)
    else:
        print(rendered, end='')


if __name__ == '__main__':
    main()
