#!/usr/bin/env python3
"""Build a separately identified current-contrast derivative of the frozen witness.

The original files are never changed. Only outlinePainted and result metadata
differ: at scale1 the current selector paints a black outer edge and a white
edge inset one pixel. The timer, ownership, input, all-display requirement,
5+30 invocations, nearest-rank p95 and 150ms budget remain byte-for-byte source.
--test runs synthetic native-pixel negative controls under Xvfb, not acceptance.
"""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[2]
FROZEN_SHA256 = 'fcb274049e4279a733b74e6fb9ec80bd4aec6389babe65dcb6497cafd6df887c'

PAINT = r'''bool outlinePainted(xcb_connection_t* c,xcb_window_t root,xcb_window_t window,const QRectF& selection){
 auto* position=xcb_translate_coordinates_reply(c,xcb_translate_coordinates(c,window,root,0,0),nullptr);
 auto* geometry=xcb_get_geometry_reply(c,xcb_get_geometry(c,window),nullptr);
 if(!position||!geometry){free(position);free(geometry);return false;}
 const QPointF origin(position->dst_x,position->dst_y);
 const auto clipped=selection.intersected(QRectF(origin,QSizeF(geometry->width,geometry->height)));
 free(position);free(geometry);
 if(clipped.width()<4||clipped.height()<4)return false;
 const auto local=clipped.translated(-origin);
 // Same scale1 top-edge location as the frozen witness; read the outer and
 // immediately inset rows together. A cover, white-only historical edge,
 // black-only partial paint, and the wrong drawable all fail this criterion.
 auto* pixels=xcb_get_image_reply(c,xcb_get_image(c,XCB_IMAGE_FORMAT_Z_PIXMAP,window,qRound(local.x())+2,qRound(local.y()),1,2,~0u),nullptr);
 if(!pixels)return false;
 const auto* bytes=xcb_get_image_data(pixels);
 const bool painted=xcb_get_image_data_length(pixels)>=8&&bytes[0]==0&&bytes[1]==0&&bytes[2]==0&&bytes[4]==255&&bytes[5]==255&&bytes[6]==255;
 free(pixels);return painted;
}
'''

TEST = r'''#define main frozen_benchmark_main
#include "bench_current_capture.cpp"
#undef main
#include <cstdio>
class Pattern:public QWidget{
public:int mode=0;Pattern():QWidget(nullptr,Qt::FramelessWindowHint|Qt::X11BypassWindowManagerHint){resize(320,240);move(0,0);}
void paintEvent(QPaintEvent*)override{QPainter p(this);p.fillRect(rect(),QColor(80,90,100));
 auto ring=[&](QRect r,int width,QColor color){p.fillRect(QRect(r.x(),r.y(),r.width(),width),color);p.fillRect(QRect(r.x(),r.bottom()-width+1,r.width(),width),color);p.fillRect(QRect(r.x(),r.y(),width,r.height()),color);p.fillRect(QRect(r.right()-width+1,r.y(),width,r.height()),color);};
 const QRect selection(40,40,200,100);
 if(mode==1||mode==2)ring(selection,3,Qt::black);
 if(mode==2||mode==4)ring(selection.adjusted(1,1,-1,-1),1,Qt::white);
 if(mode==3)ring(selection,3,Qt::white);
 if(mode==5){ring(selection.translated(20,20),3,Qt::black);ring(selection.translated(20,20).adjusted(1,1,-1,-1),1,Qt::white);}
}};
int main(int argc,char** argv){QApplication app(argc,argv);Pattern fixture;
 auto* connection=xcb_connect(nullptr,nullptr);if(xcb_connection_has_error(connection))return 2;
 const auto root=xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;
 fixture.show();auto settle=[&]{for(int i=0;i<30;++i){QCoreApplication::processEvents();QThread::msleep(2);}};settle();
 for(int mode=0;mode<6;++mode){fixture.mode=mode;fixture.update();settle();const bool actual=outlinePainted(connection,root,fixture.winId(),QRectF(40,40,200,100));if(actual!=(mode==2)){std::fprintf(stderr,"paint control %d failed\n",mode);return 1;}}
 // Reuse the unmodified exact-helper ownership predicate, with real proc
 // identities. Positive paint must never admit the wrong executable/parent.
 struct stat expected{},wrong{};if(::stat("/proc/self/exe",&expected)||::stat("/bin/false",&wrong))return 2;
 ProcessStamp stamp;if(!captureChild(::getpid(),::getppid(),expected,stamp)||captureChild(::getpid(),::getpid(),expected,stamp)||captureChild(::getpid(),::getppid(),wrong,stamp))return 1;
 Pattern wrongWindow;wrongWindow.show();settle();if(outlinePainted(connection,root,wrongWindow.winId(),QRectF(40,40,200,100)))return 1;
 xcb_disconnect(connection);std::puts("current contrast positive; cover/no-overlay, black-only partial, old white, white-only partial, wrong position/window/executable/parent negatives passed");return 0;
}
'''

CMAKE = '''cmake_minimum_required(VERSION 3.24)
project(putgd_current_capture_witness LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 6.4.2 REQUIRED COMPONENTS Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb xcb-keysyms xcb-xtest)
add_executable(putgd-bench-current-capture bench_current_capture.cpp)
add_executable(test-current-capture test_current_capture.cpp)
foreach(target putgd-bench-current-capture test-current-capture)
  target_link_libraries(${target} PRIVATE Qt6::Widgets PkgConfig::XCB)
endforeach()
'''


def generate(source):
    if hashlib.sha256(source).hexdigest() != FROZEN_SHA256:
        raise ValueError('frozen_source_hash_mismatch')
    original = source.decode()
    begin, end = original.index('bool outlinePainted('), original.index('bool visibleCanvas(')
    value = original[:begin] + PAINT + original[end:]
    value = value.replace('result["schemaVersion"]=3;', 'result["schemaVersion"]=4;result["frozenSourceSha256"]="' + FROZEN_SHA256 + '";')
    value = value.replace('result["witnessMode"]="live-outline";', 'result["witnessMode"]="current-contrast";result["paintCriterion"]="scale1 black outer edge plus white inset edge at the original top-edge position";')
    return value, ''.join(difflib.unified_diff(original.splitlines(True), value.splitlines(True),
                                             fromfile='frozen/bench_capture.cpp', tofile='current/bench_current_capture.cpp'))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--source-revision', required=True, help='Full separately identified driver source commit')
    parser.add_argument('--build', action='store_true')
    parser.add_argument('--test', action='store_true')
    args = parser.parse_args(argv)
    import re
    if not re.fullmatch('[0-9a-f]{40}', args.source_revision):
        parser.error('full source revision required')
    os.umask(0o077)
    root = args.output.resolve()
    root.mkdir(mode=0o700, exist_ok=False)
    original = REPO / 'tests/performance/bench_capture.cpp'
    source, difference = generate(original.read_bytes())
    for name, text in [('bench_current_capture.cpp', source), ('test_current_capture.cpp', TEST),
                       ('CMakeLists.txt', CMAKE), ('frozen-vs-current.diff', difference)]:
        (root / name).write_text(text)
    metadata = {'kind': 'current-contrast', 'sourceRevision': args.source_revision,
        'frozenSourceSha256': FROZEN_SHA256, 'generatorSha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'generatedSourceSha256': hashlib.sha256(source.encode()).hexdigest(), 'status': 'generated',
        'warmupInvocations': 5, 'sampleCount': 30, 'p95LimitMs': 150,
        'scope': 'separate version-correct paint predicate; unchanged frozen timer/ownership/input/all-display/cancellation/sample/budget source',
        'negativeControlsAreProductAcceptance': False}
    def save():
        (root / 'witness.json').write_text(json.dumps(metadata, indent=2) + '\n')
    save()
    def command(arguments, env=None):
        metadata.setdefault('commands', []).append(arguments)
        save()
        with (root / 'build.log').open('ab') as log:
            subprocess.run(arguments, check=True, stdout=log, stderr=log, env=env, timeout=180)
    try:
        if args.build or args.test:
            command(['cmake', '-S', str(root), '-B', str(root / 'build'), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release'])
            command(['nice', '-n', '10', 'cmake', '--build', str(root / 'build'), '--parallel', '1'])
            binary = root / 'build/putgd-bench-current-capture'
            metadata.update(status='built', binary=str(binary), binarySha256=hashlib.sha256(binary.read_bytes()).hexdigest())
        if args.test:
            with tempfile.TemporaryDirectory(prefix='p379-x-') as private:
                runtime = Path(private) / 'r'; runtime.mkdir(mode=0o700)
                command([sys.executable, str(REPO / 'tools/diagnostics/check_socket_paths.py'), '--socket', str(runtime / 'putgd/session.sock')])
                command(['xvfb-run', '-a', str(root / 'build/test-current-capture')],
                        {**os.environ, 'QT_QPA_PLATFORM': 'xcb', 'XDG_RUNTIME_DIR': str(runtime)})
            metadata['syntheticNegativeControlsPassed'] = True
        save()
        return 0
    except BaseException as error:
        metadata.update(status='failed', errorType=type(error).__name__)
        save()
        raise


if __name__ == '__main__':
    raise SystemExit(main())
