from pathlib import Path
import difflib,json,hashlib
root=Path('/p3/prepared');bun=root/'bun';webkit=root/'WebKit';proof=Path('/p3/proof');proof.mkdir(exist_ok=True)
changes=[]
def change(path,after):
 before=path.read_text();assert 'putgd_jsc_rebuild_proof' not in before and 'putgd_tcc_rebuild_proof' not in before,'proof markers already applied';(proof/(path.name+'.upstream')).write_text(before);path.write_text(after(before))
 patch=''.join(difflib.unified_diff(before.splitlines(True),path.read_text().splitlines(True),fromfile='a/'+str(path.relative_to(root)),tofile='b/'+str(path.relative_to(root))))
 changes.append({'path':str(path.relative_to(root)),'beforeSha256':hashlib.sha256(before.encode()).hexdigest(),'afterSha256':hashlib.sha256(path.read_bytes()).hexdigest(),'patch':patch})
def rust(text):
 anchor='pub(crate) fn print_revision_and_exit() -> ! {'
 assert text.count(anchor)==1
 return text.replace(anchor,anchor+'''
    unsafe extern "C" {
        fn putgd_jsc_rebuild_proof() -> *const core::ffi::c_char;
        fn putgd_tcc_rebuild_proof() -> *const core::ffi::c_char;
    }
    let proof_writer = Output::writer();
    let _ = proof_writer.write_all(b"putgd-bun-native-proof ");
    unsafe {
        let _ = proof_writer.write_all(std::ffi::CStr::from_ptr(putgd_jsc_rebuild_proof()).to_bytes());
        let _ = proof_writer.write_all(b" ");
        let _ = proof_writer.write_all(std::ffi::CStr::from_ptr(putgd_tcc_rebuild_proof()).to_bytes());
    }
    let _ = proof_writer.write_all(b"\\n");''',1)
change(bun/'src/runtime/cli/mod.rs',rust)
change(webkit/'Source/JavaScriptCore/API/JSBase.cpp',lambda s:s+'\nextern "C" const char* putgd_jsc_rebuild_proof(void) { return "putgd-jsc-source-proof"; }\n')
change(bun/'vendor/tinycc/libtcc.c',lambda s:s+'\nconst char* putgd_tcc_rebuild_proof(void) { return "putgd-tinycc-source-proof"; }\n')
(proof/'source-changes.json').write_text(json.dumps(changes,indent=2)+'\n')
(proof/'native-modification.patch').write_text(''.join(x['patch'] for x in changes))
print(json.dumps({'sourceModifications':len(changes),'purpose':'Harmless --revision markers linked from Bun Rust, JavaScriptCore C++ and TinyCC C; proof artifacts only, never production helper'}))
