#!/usr/bin/env python3
"""Checks that the newest version of every package in an Andromeda repository index installs with the newest of all
the others: each dependency on a package or virtual name that the index provides must be met by the newest versions.
Dependencies on names the index does not provide are left to the distro, except our own (andromeda*).
  tools/repo-check.py arch andromeda.db.tar.gz
  tools/repo-check.py apt Packages
Exit 1 lists every unmet dependency.
  tools/repo-check.py arch|apt INDEX --replaced
prints "<name> <file>" for each entry that another entry replaces outright (arch: replaces; apt: Replaces and
Conflicts), without a version: the index must not list it."""

import re
import subprocess
import sys
import tarfile


def vercmp(kind, a, b):
    if a == b:
        return 0
    if kind == "arch":
        return int(subprocess.run(["vercmp", a, b], capture_output=True, text=True, check=True).stdout)
    return deb_vercmp(a, b)


def deb_vercmp(a, b):
    """Debian policy 5.6.12: epoch, then upstream and revision compared by alternating non-digit and digit runs."""

    def split(v):
        epoch, _, rest = v.rpartition(":") if ":" in v else ("0", "", v)
        upstream, _, revision = rest.rpartition("-") if "-" in rest else (rest, "", "0")
        return int(epoch or 0), upstream, revision

    def order(c):
        return 0 if c == "~" else (ord(c) if c.isalpha() else ord(c) + 256) if c else 1

    def part(x, y):
        while x or y:
            xs = re.match(r"[^\d]*", x).group()
            ys = re.match(r"[^\d]*", y).group()
            for i in range(max(len(xs), len(ys))):
                cx = order(xs[i]) if i < len(xs) else (0 if i < len(ys) and ys[i] == "~" else 1)
                cy = order(ys[i]) if i < len(ys) else (0 if i < len(xs) and xs[i] == "~" else 1)
                if i >= len(xs):
                    cx = 1
                if i >= len(ys):
                    cy = 1
                if cx != cy:
                    return -1 if cx < cy else 1
            x, y = x[len(xs):], y[len(ys):]
            xd = re.match(r"\d*", x).group()
            yd = re.match(r"\d*", y).group()
            if int(xd or 0) != int(yd or 0):
                return -1 if int(xd or 0) < int(yd or 0) else 1
            x, y = x[len(xd):], y[len(yd):]
        return 0

    ea, ua, ra = split(a)
    eb, ub, rb = split(b)
    if ea != eb:
        return -1 if ea < eb else 1
    return part(ua, ub) or part(ra, rb)


def satisfies(kind, have, op, want):
    if not op:
        return True
    c = vercmp(kind, have, want)
    return {"=": c == 0, ">=": c >= 0, "<=": c <= 0, ">": c > 0, "<": c < 0, ">>": c > 0, "<<": c < 0}[op]


def arch_packages(path):
    """(name, version, depends, provides, conflicts, replaces, file); depends etc. as (name, op, version)."""
    packages = []
    with tarfile.open(path) as db:
        for member in db.getmembers():
            if not member.name.endswith("/desc"):
                continue
            fields, key = {}, None
            for line in db.extractfile(member).read().decode().splitlines():
                if line.startswith("%") and line.endswith("%"):
                    key = line.strip("%")
                    fields[key] = []
                elif line and key:
                    fields[key].append(line)
            split = lambda s: re.match(r"([^<>=]+)(?:(<=|>=|=|<|>)(.+))?$", s).groups()
            packages.append((fields["NAME"][0], fields["VERSION"][0], [split(d) for d in fields.get("DEPENDS", [])],
                             [split(p) for p in fields.get("PROVIDES", [])],
                             [split(c) for c in fields.get("CONFLICTS", []) + fields.get("BREAKS", [])],
                             [split(r) for r in fields.get("REPLACES", [])], fields["FILENAME"][0]))
    return packages


def apt_packages(path):
    def relations(text):
        out = []
        for alternative in filter(None, (part.strip() for part in text.split(","))):
            # An alternative group (a | b) counts as met when its first choice is met.
            m = re.match(r"([^\s(|]+)(?:\s*\((<<|<=|=|>=|>>)\s*([^)]+)\))?", alternative.split("|")[0].strip())
            out.append(m.groups())
        return out

    packages = []
    for stanza in open(path).read().split("\n\n"):
        fields = dict(re.findall(r"^([A-Za-z-]+): (.*)$", stanza, re.M))
        if "Package" not in fields:
            continue
        packages.append((fields["Package"], fields["Version"], relations(fields.get("Depends", "") + "," +
                         fields.get("Pre-Depends", "")), relations(fields.get("Provides", "")),
                         relations(fields.get("Breaks", "") + "," + fields.get("Conflicts", "")),
                         [r for r in relations(fields.get("Replaces", "")) if r in relations(fields.get("Conflicts", ""))],
                         fields.get("Filename", "")))
    return packages


def main():
    kind, path = sys.argv[1], sys.argv[2]
    packages = arch_packages(path) if kind == "arch" else apt_packages(path)
    if sys.argv[3:] == ["--replaced"]:
        gone = {r for p in packages for r, op, _ in p[5] if not op and r != p[0]}
        for p in packages:
            if p[0] in gone:
                print(p[0], p[6])
        return
    newest = {}
    for package in packages:
        # A debug symbol package pins its exact package; a missing newer one only means no symbols.
        if package[0].endswith("-dbgsym"):
            continue
        if package[0] not in newest or vercmp(kind, package[1], newest[package[0]][1]) > 0:
            newest[package[0]] = package
    # name -> versions that provide it among the newest packages (None: provided without a version).
    provided = {}
    for name, version, _, provides, *_ in newest.values():
        provided.setdefault(name, []).append(version)
        for p, _, v in provides:
            provided.setdefault(p, []).append(v)
    problems = []
    for name, version, depends, _, conflicts, *_ in sorted(newest.values()):
        for dep, op, want in depends:
            # A name the index does not provide is the distro's, unless it is one of ours (andromeda*).
            if dep not in provided and "andromeda" not in dep:
                continue
            if not any(v is not None and satisfies(kind, v, op, want) or (v is None and not op)
                       for v in provided.get(dep, [])):
                problems.append(f"{name} {version} needs {dep}{' ' + op + ' ' + want if op else ''}; "
                                f"the index offers {', '.join(v or '(unversioned)' for v in provided.get(dep, [])) or 'nothing'}")
        for dep, op, want in conflicts:
            for other in newest.values():
                if other[0] == dep and other[0] != name and satisfies(kind, other[1], op, want):
                    problems.append(f"{name} {version} breaks {other[0]} {other[1]}")
    for problem in problems:
        print(problem)
    print(f"repo-check: {len(newest)} packages, {len(problems)} unmet", file=sys.stderr)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
