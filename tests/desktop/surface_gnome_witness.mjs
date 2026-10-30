import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
const path=process.argv[2] ?? new URL('../../tools/lab/surface_driver/compositor/gnome-witness/extension.js',import.meta.url);
let code=fs.readFileSync(path,'utf8').replace(/^import .*;\n/gm,'').replace('export default class LabWitness','class LabWitness');
const callbacks=new Map();
let unexported=false;
const native={display:{connect:(signal,cb)=>(callbacks.set(signal,cb),signal),disconnect:id=>callbacks.delete(id)}};
const fake={global:native,Atk:{Object:{prototype:{get_name(){return this.objectName;},get_role(){return this.objectRole;}}}},GLib:{uuid_string_random:()=> 'epoch',get_monotonic_time:()=>1000},
 Gio:{DBusExportedObject:{wrapJSObject:()=>({export(){},unexport(){unexported=true}})},DBus:{session:{}}},
 Meta:{GrabOp:{MOVING:1,RESIZING_SE:24577}},Extension:class {}};
vm.createContext(fake);vm.runInContext(code+'\nthis.Witness=LabWitness;',fake);
const w=new fake.Witness();w.enable();
const window={get_id:()=>11,get_pid:()=>50};
callbacks.get('grab-op-begin')(native.display,window,24577);
callbacks.get('grab-op-end')(native.display,window,24577);
let snapshot=JSON.parse(w.Grabs());
assert.equal(snapshot.epoch,'epoch');assert.equal(snapshot.events[0].kind,'resize');
assert.equal(snapshot.events[0].operation,'RESIZING_SE');assert.equal(snapshot.events[0].window_id,11);
assert.equal(snapshot.events[0].phase,'begin');assert.equal(snapshot.events[1].phase,'end');
for(let i=0;i<300;i++)callbacks.get('grab-op-begin')(native.display,window,1);
snapshot=JSON.parse(w.Grabs());assert.equal(snapshot.events.length,256);assert.equal(snapshot.serial,302);
w.disable();assert.equal(callbacks.size,0);assert.equal(unexported,true);
// StLabel's accessible object supplies its role/name even when the actor's
// override properties still contain their defaults. Match the exported ATK
// object, as the live AT-SPI banner does (#926).
const labelActor = {
    get_transformed_position: () => [830, 30], get_transformed_size: () => [200, 20],
    get_accessible_name: () => null, accessible_role: 0,
    get_text: () => 'Upload complete',
    get_accessible: () => ({objectName: 'Upload complete', objectRole: 28,
        get_name: () => {throw new TypeError('Atk.Action.get_name needs an index');}}),
};
fake.labelActor = labelActor;
const description = vm.runInContext('actorDescription(labelActor)', fake);
assert.equal(description.role, 28);
assert.equal(description.accessible, 'Upload complete');
console.log('GNOME witness signal lifecycle passed');
