#!/usr/bin/env python3
"""AppStream catalog data for the pkg.put.gd repositories (#314).

The metainfo the packages install lets a software centre show put.gd once it's
installed. To find and install it from a search, GNOME Software, Discover and
Mint's Software Manager need a catalog in the repository itself. The signer
builds one from the exact package each suite or repository carries, before
anything is signed:

- apt: DEP-11, `dists/<channel>/main/dep11/Components-amd64.yml.gz` and
  `icons-<size>.tar.gz`, listed in `Release` so `InRelease` covers them. apt
  fetches them through the targets appstream's `50appstream` declares, and
  `appstreamcli refresh` installs them.
- dnf: `appstream` and `appstream-icons` entries in `repomd.xml`, laid out as
  copr repositories carry them. dnf5 installs them under /var/cache/swcatalog
  for the repository id.

`appstreamcli compose` (package appstream-compose) reads the package's
metainfo, desktop file and icon and renders the cached icons;
`appstreamcli convert` writes the DEP-11 YAML; `appstreamcli validate` checks
the catalog and the YAML read back before either is used.
"""
import gzip
import hashlib
import io
import os
from pathlib import Path
import posixpath
import re
import shutil
import subprocess
import tarfile
import xml.etree.ElementTree as ET

COMPONENT = 'gd.put.Putgd'
PACKAGE = 'putgd'
DEP11 = 'main/dep11'
COMPONENTS = 'Components-amd64.yml.gz'
# Only what compose reads; the rest of the package stays packed.
INPUTS = ('usr/share/metainfo/', 'usr/share/applications/', 'usr/share/icons/', 'usr/share/pixmaps/')
MAX_INPUT = 16 * 1024 * 1024
ICON_SIZE = re.compile(r'^[0-9]+x[0-9]+(@[0-9])?$')
RELEASE_HASHES = {'MD5Sum': 'md5', 'SHA1': 'sha1', 'SHA256': 'sha256', 'SHA512': 'sha512'}


class CatalogError(ValueError):
    pass


def unpack(package, destination, run=subprocess.run):
    """The package's AppStream inputs (metainfo, desktop files, icons), extracted as plain files."""
    package, destination = Path(package), Path(destination)
    archive = destination.with_name(destination.name + '.tar')
    with open(archive, 'wb') as out:
        if package.suffix == '.deb':
            run(['dpkg-deb', '--fsys-tarfile', str(package)], stdout=out, check=True)
        elif package.suffix == '.rpm':
            with open(package, 'rb') as source:
                run(['rpm2archive', '-n', '-'], stdin=source, stdout=out, check=True)
        else:
            raise CatalogError('Not a DEB or RPM: ' + package.name)
    destination.mkdir(parents=True)
    with tarfile.open(archive) as tar:
        for member in tar:
            name = posixpath.normpath(member.name).lstrip('/')
            if not member.isfile() or not name.startswith(INPUTS):
                continue
            if member.size > MAX_INPUT:
                raise CatalogError('Unexpectedly large AppStream input: ' + name)
            member.name = name
            tar.extract(member, destination, filter='data')
    archive.unlink()
    return destination


def compose(package, origin, workdir, run=subprocess.run):
    """Compose the catalog for one package: (catalog XML text, {icon size: directory})."""
    workdir = Path(workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    root = unpack(package, workdir / 'root', run)
    result = workdir / 'result'
    # The signer's unit makes everything but its state read-only; compose's scratch files stay here.
    env = dict(os.environ, TMPDIR=str(workdir), XDG_CACHE_HOME=str(workdir / 'cache'))
    run(['appstreamcli', 'compose', '--no-net', '--no-color', '--print-report=short', '--origin=' + origin,
         '--result-root=' + str(result), '--data-dir=' + str(result / 'data'), '--icons-dir=' + str(result / 'icons'),
         '--hints-dir=' + str(result / 'hints'), '--components=' + COMPONENT, str(root)],
        check=True, capture_output=True, text=True, env=env)
    data = result / 'data' / (origin + '.xml.gz')
    if not data.is_file():
        raise CatalogError(f'appstreamcli compose found no {COMPONENT} in {Path(package).name}')
    icons = {path.name: path for path in sorted((result / 'icons').iterdir())
             if path.is_dir() and ICON_SIZE.match(path.name)} if (result / 'icons').is_dir() else {}
    return finish(gzip.decompress(data.read_bytes()).decode(), origin, icons), icons


def collapse(element):
    """Join a paragraph's lines the way a software centre would show them."""
    for node in element.iter():
        if node.tag in ('p', 'li') and len(node) == 0 and node.text:
            node.text = ' '.join(node.text.split())


def finish(text, origin, icons):
    """The composed catalog with the origin and package name the repository needs; exactly put.gd."""
    root = ET.fromstring(text)
    if root.tag != 'components':
        raise CatalogError('Not an AppStream catalog')
    components = root.findall('component')
    if [component.findtext('id') for component in components] != [COMPONENT]:
        raise CatalogError('The catalog must describe exactly ' + COMPONENT)
    component = components[0]
    root.set('origin', origin)
    for old in component.findall('pkgname'):
        component.remove(old)
    pkgname = ET.Element('pkgname')
    pkgname.text = PACKAGE
    component.insert(list(component).index(component.find('id')) + 1, pkgname)
    for description in component.findall('description'):
        collapse(description)
    for icon in component.findall('icon'):
        if icon.get('type') != 'cached':
            continue
        size = f'{icon.get("width")}x{icon.get("height")}' + (f'@{icon.get("scale")}' if icon.get('scale', '1') != '1' else '')
        if size not in icons or not (icons[size] / (icon.text or '')).is_file():
            raise CatalogError(f'The catalog names a {size} icon compose did not render')
    if not any(icon.get('type') == 'cached' for icon in component.findall('icon')):
        raise CatalogError('The catalog has no cached icon')
    ET.indent(root)
    return '<?xml version="1.0" encoding="utf-8"?>\n' + ET.tostring(root, encoding='unicode') + '\n'


def validate(path, run=subprocess.run):
    result = run(['appstreamcli', 'validate', '--no-net', '--no-color', str(path)], capture_output=True, text=True)
    if result.returncode != 0:
        raise CatalogError(f'appstreamcli validate rejects {Path(path).name}: ' + (result.stdout + result.stderr).strip()[-800:])


def check_catalog(path, run=subprocess.run):
    """Valid, and still exactly put.gd from package putgd."""
    validate(path, run)
    root = ET.parse(path).getroot()
    if [(c.findtext('id'), c.findtext('pkgname')) for c in root.findall('component')] != [(COMPONENT, PACKAGE)]:
        raise CatalogError(f'{Path(path).name} does not describe exactly {COMPONENT} in package {PACKAGE}')


def gzip_bytes(data):
    """Deterministic gzip: the same catalog uploads once, not on every run."""
    buffer = io.BytesIO()
    with gzip.GzipFile(filename='', mode='wb', fileobj=buffer, mtime=0, compresslevel=9) as stream:
        stream.write(data)
    return buffer.getvalue()


def tar_bytes(files):
    """Deterministic, uncompressed tar of {name: path}, root-owned and dated 1970."""
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode='w', format=tarfile.USTAR_FORMAT) as tar:
        for name, path in sorted(files.items()):
            data = Path(path).read_bytes()
            info = tarfile.TarInfo(name)
            info.size, info.mode, info.mtime = len(data), 0o644, 0
            tar.addfile(info, io.BytesIO(data))
    return buffer.getvalue()


def write_dep11(package, origin, dists, workdir, run=subprocess.run):
    """DEP-11 for one suite, replacing main/dep11/; returns {path relative to dists: sha256}."""
    dists, workdir = Path(dists), Path(workdir)
    catalog, icons = compose(package, origin, workdir, run)
    xml = workdir / 'catalog.xml'
    xml.write_text(catalog)
    check_catalog(xml, run)
    yml = workdir / 'Components-amd64.yml'
    run(['appstreamcli', 'convert', '--no-color', '--format=yaml', str(xml), str(yml)], check=True, capture_output=True, text=True)
    # The YAML apt serves is checked as the clients will read it.
    back = workdir / 'roundtrip.xml'
    run(['appstreamcli', 'convert', '--no-color', '--format=xml', str(yml), str(back)], check=True, capture_output=True, text=True)
    check_catalog(back, run)
    if not yml.read_text().startswith('---\nFile: DEP-11\n') or f'\nOrigin: {origin}\n' not in yml.read_text():
        raise CatalogError('appstreamcli convert wrote an unexpected DEP-11 header')
    target = dists / DEP11
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    (target / COMPONENTS).write_bytes(gzip_bytes(yml.read_bytes()))
    for size, directory in icons.items():
        # DEP-11 icon tarballs are flat; appstream unpacks each into icons/<origin>/<size>/.
        files = {path.name: path for path in directory.iterdir() if path.is_file()}
        (target / f'icons-{size}.tar.gz').write_bytes(gzip_bytes(tar_bytes(files)))
    return {path.relative_to(dists).as_posix(): sha256(path) for path in sorted(target.iterdir())}


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def list_in_release(release, dists, relatives, prefix=DEP11 + '/'):
    """List these files (relative to dists) under every hash section of Release, replacing what
    was listed under prefix before; reprepro's own entries stay byte for byte."""
    out, section = [], None

    def flush():
        if section:
            for relative in relatives:
                data = (Path(dists) / relative).read_bytes()
                out.append(f' {hashlib.new(RELEASE_HASHES[section], data).hexdigest()} {len(data)} {relative}')

    for line in Path(release).read_text().splitlines():
        if line.startswith(' ') and section:
            if not line.split()[-1].startswith(prefix):
                out.append(line)
            continue
        flush()
        section = line[:-1] if line.endswith(':') and line[:-1] in RELEASE_HASHES else None
        out.append(line)
    flush()
    if not any(line[:-1] in RELEASE_HASHES for line in out if line.endswith(':')):
        raise CatalogError('Release has no hash sections')
    Path(release).write_text('\n'.join(out) + '\n')


def repomd_locations(repomd):
    namespace = {'repo': 'http://linux.duke.edu/metadata/repo'}
    root = ET.parse(repomd).getroot()
    return {data.get('type'): data.find('repo:location', namespace).get('href') for data in root.findall('repo:data', namespace)}


def write_appstream_repodata(package, origin, repo, workdir, run=subprocess.run):
    """appstream and appstream-icons in repo/repodata for one RPM; returns {path relative to repo: sha256}."""
    repo, workdir = Path(repo), Path(workdir)
    catalog, icons = compose(package, origin, workdir, run)
    out = workdir / 'repodata-input'
    out.mkdir()
    xml = out / 'appstream.xml'
    xml.write_text(catalog)
    check_catalog(xml, run)
    # Like copr: one tarball of <size>/<icon>, which dnf5 unpacks into icons/<repo id>/.
    tar = out / 'appstream-icons.tar'
    tar.write_bytes(tar_bytes({f'{size}/{path.name}': path for size, directory in icons.items()
                               for path in directory.iterdir() if path.is_file()}))
    repodata = repo / 'repodata'
    for mdtype, path in (('appstream', xml), ('appstream-icons', tar)):
        run(['modifyrepo_c', '--mdtype=' + mdtype, '--compress-type=gz', '--checksum=sha256', '--unique-md-filenames',
             str(path), str(repodata)], check=True, capture_output=True, text=True)
    locations = repomd_locations(repodata / 'repomd.xml')
    if not {'appstream', 'appstream-icons'} <= set(locations):
        raise CatalogError('repomd.xml does not list the AppStream metadata')
    # Earlier runs' catalog files; createrepo_c prunes its own types.
    for path in repodata.glob('*appstream*'):
        if 'repodata/' + path.name not in locations.values():
            path.unlink()
    return {href: sha256(repo / href) for kind, href in sorted(locations.items()) if kind.startswith('appstream')}
