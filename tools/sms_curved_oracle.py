"""Independent double-precision curved glass oracle (not production code).

Solves a receiver -> two sphere interfaces -> light chain with NumPy, then
finite-differences the receiver solid-angle Jacobian per unit light area.
"""
import json
from pathlib import Path
import numpy as np

R = 0.5
CENTER = np.array([0.0, 0.5, 0.0])
RECEIVER = np.array([-1.2, 0.0, -0.3])
LIGHT = np.array([1.5, 2.5, 0.8])
ETA_INTO = 1.5


def point(theta, phi):
    return CENTER + R * np.array([np.sin(theta) * np.cos(phi), np.cos(theta), np.sin(theta) * np.sin(phi)])


def normal(theta, phi):
    return np.array([np.sin(theta) * np.cos(phi), np.cos(theta), np.sin(theta) * np.sin(phi)])


def tangents(theta, phi):
    dtheta = np.array([np.cos(theta) * np.cos(phi), -np.sin(theta), np.cos(theta) * np.sin(phi)])
    dphi = np.array([-np.sin(phi), 0.0, np.cos(phi)])
    return dtheta, dphi


INVALID = np.array([1.0e6, 1.0e6, 1.0e6, 1.0e6])


def residual(z, target=LIGHT):
    p1 = point(z[0], z[1])
    p2 = point(z[2], z[3])
    a1 = RECEIVER - p1
    b1 = p2 - p1
    t1 = tangents(z[0], z[1])
    h1 = a1 / np.linalg.norm(a1) + ETA_INTO * b1 / np.linalg.norm(b1)
    if not (np.all(np.isfinite(h1)) and np.all(np.isfinite(t1[0])) and np.all(np.isfinite(t1[1]))):
        return INVALID
    r1 = np.array([np.dot(h1, t1[0]), np.dot(h1, t1[1])])
    a2 = p1 - p2
    b2 = target - p2
    t2 = tangents(z[2], z[3])
    h2 = a2 / np.linalg.norm(a2) + b2 / np.linalg.norm(b2) / ETA_INTO
    if not (np.all(np.isfinite(h2)) and np.all(np.isfinite(t2[0])) and np.all(np.isfinite(t2[1]))):
        return INVALID
    r2 = np.array([np.dot(h2, t2[0]), np.dot(h2, t2[1])])
    return np.concatenate([r1, r2])


def solve(z0, target=LIGHT, iterations=128):
    z = np.array(z0, dtype=float)
    for _ in range(iterations):
        r = residual(z, target)
        if np.linalg.norm(r) < 2.0e-13:
            return z, True
        j = np.empty((4, 4))
        eps = 1.0e-7
        for column in range(4):
            plus = z.copy()
            plus[column] += eps
            minus = z.copy()
            minus[column] -= eps
            j[:, column] = (residual(plus, target) - residual(minus, target)) / (2.0 * eps)
        step = np.linalg.lstsq(j, -r, rcond=None)[0]
        scale = 1.0
        old = np.linalg.norm(r)
        while scale > 1.0e-5:
            candidate = z + scale * step
            candidate_norm = np.linalg.norm(residual(candidate, target))
            if np.isfinite(candidate_norm) and candidate_norm < old:
                z = candidate
                break
            scale *= 0.5
        else:
            return z, False
    return z, np.linalg.norm(residual(z, target)) < 2.0e-13


def solve_light_offset(offset, starts):
    target = LIGHT + offset
    best = None
    for start in starts:
        z, ok = solve(start, target)
        if ok and (best is None or np.linalg.norm(residual(z, target)) < np.linalg.norm(residual(best, target))):
            best = z
    assert best is not None, f'No independent root for offset {offset}'
    return best, target


def solid_angle_direction(p1):
    direction = p1 - RECEIVER
    return direction / np.linalg.norm(direction)


base_starts = []
for theta1 in np.linspace(0.7, 2.2, 9):
    for phi1 in np.linspace(-np.pi, np.pi, 13):
        for theta2 in np.linspace(0.7, 2.2, 5):
            for phi2 in np.linspace(-np.pi, np.pi, 7):
                base_starts.append([theta1, phi1, theta2, phi2])
z, target = solve_light_offset(np.zeros(3), base_starts)
p1, p2 = point(z[0], z[1]), point(z[2], z[3])
eps = 2.0e-6
jacobian = []
for offset in (np.array([eps, 0.0, 0.0]), np.array([0.0, eps, 0.0])):
    z_plus, target_plus = solve_light_offset(offset, [z])
    z_minus, target_minus = solve_light_offset(-offset, [z])
    jacobian.append((solid_angle_direction(point(z_plus[0], z_plus[1])) -
                     solid_angle_direction(point(z_minus[0], z_minus[1]))) / (2.0 * eps))
jacobian = np.stack(jacobian, axis=1)
expected_geometric = float(np.sqrt(np.linalg.det(jacobian.T @ jacobian)))
normal1 = normal(z[0], z[1])
normal2 = normal(z[2], z[3])
tangent1 = tangents(z[0], z[1])[0]
tangent2 = tangents(z[2], z[3])[0]
receipt = {
    'sphere_radius': R,
    'center': CENTER.tolist(),
    'receiver': RECEIVER.tolist(),
    'light': LIGHT.tolist(),
    'angles': z.tolist(),
    'point1': p1.tolist(),
    'point2': p2.tolist(),
    'normal1': normal1.tolist(),
    'normal2': normal2.tolist(),
    'tangent1': tangent1.tolist(),
    'tangent2': tangent2.tolist(),
    'residual_norm': float(np.linalg.norm(residual(z))),
    'expected_geometric': expected_geometric,
    'jacobian': jacobian.tolist(),
}
Path(r'C:\Users\norbe\AppData\Local\Temp\opencode\sms-c1-outward').mkdir(parents=True, exist_ok=True)
Path(r'C:\Users\norbe\AppData\Local\Temp\opencode\sms-c1-outward\curved_oracle.json').write_text(
    json.dumps(receipt, indent=2), encoding='utf-8')
print(json.dumps(receipt, indent=2))
