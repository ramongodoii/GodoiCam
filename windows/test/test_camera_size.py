"""Confere pickCameraSize com as saídas 16:9 do S24 (sem Android)."""

# Cópia da regra em CameraCaps.pickCameraSize — manter igual ao Kotlin.
def pick(covering, width, height, cover_portrait):
    if not covering:
        return None
    budget = width * height * 9 // 4
    within = [c for c in covering if c[0] * c[1] <= budget] or covering
    if not cover_portrait:
        return min(within, key=lambda c: c[0] * c[1])
    need_h = min(width, height * 4 // 3)
    need = [c for c in within if c[1] >= need_h]
    return min(need, key=lambda c: c[0] * c[1]) if need else min(within, key=lambda c: c[0] * c[1])


S24 = [(4000, 2252), (3840, 2160), (2560, 1440), (1920, 1080), (1280, 720)]


def covering(w, h):
    return [c for c in S24 if c[0] >= w and c[1] >= h]


cases = [
    ("1080p deitado", covering(1920, 1080), 1920, 1080, False, (1920, 1080)),
    ("1080p em pé", covering(1920, 1080), 1920, 1080, True, (2560, 1440)),
    ("720p deitado", covering(1280, 720), 1280, 720, False, (1280, 720)),
    ("720p em pé", covering(1280, 720), 1280, 720, True, (1920, 1080)),
    ("4K", covering(3840, 2160), 3840, 2160, True, (3840, 2160)),
]

failed = 0
for name, cov, w, h, portrait, expected in cases:
    got = pick(cov, w, h, portrait)
    ok = got == expected
    failed += 0 if ok else 1
    print(f"{'OK' if ok else 'FAIL'} {name}: {got} (esperado {expected})")

# 4K nunca entra no 1080p.
got = pick(covering(1920, 1080), 1920, 1080, True)
if got and got[1] >= 2160:
    print("FAIL 1080p em pé não pode ser 4K")
    failed += 1

raise SystemExit(failed)
