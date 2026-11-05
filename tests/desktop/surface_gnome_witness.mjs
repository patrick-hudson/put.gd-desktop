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

// A selector belongs to the extension's exact request/actor, never its name.
const child={}, foreign={};
let picked=child, owner=':1.50';
const selector={mapped:true,visible:true,_closed:false,_grab:{},
    contains:actor=>actor===child,
    get_transformed_position:()=>[0,0],get_transformed_size:()=>[1280,800],
    get_accessible:()=>({objectName:'Capture region',objectRole:1})};
const request={id:'request-1',sender:owner,selector,stage:'region-select',action:'region'};
const extension={_enabled:true,_selector:selector,_requests:new Map([[request.id,request]])};
fake.Main={sessionMode:{isLocked:false},extensionManager:{lookup:()=>({stateObj:extension})}};
w.metadata={version:7};
let overviewHides=0;
fake.Main.overview={visible:true,animationInProgress:false,hide(){overviewHides++;this.visible=false;}};
assert.equal(JSON.parse(w.Desktop()).overviewVisible,true);
fake.Main.sessionMode.isLocked=true;
assert.equal(w.HideOverview(),false);assert.equal(overviewHides,0);
assert.equal(JSON.parse(w.Desktop()).locked,true);
fake.Main.sessionMode.isLocked=false;
assert.equal(w.HideOverview(),true);assert.equal(overviewHides,1);
assert.deepEqual(JSON.parse(w.Desktop()),{overviewVisible:false,overviewAnimating:false,locked:false,witnessVersion:7,nativeHelperEnabled:true});
fake.GLib.Variant=class{};fake.GLib.VariantType=class{};
fake.Gio.DBusCallFlags={NONE:0};fake.Clutter={PickMode:{REACTIVE:1}};
fake.Gio.DBus.session.call_sync=(_bus,_path,_iface,method)=>({deep_unpack:()=>[method==='GetNameOwner'?owner:50]});
fake.global.stage={get_actor_at_pos:()=>picked,get_key_focus:()=>child};
let result=JSON.parse(w.Selector('{"point":[40,50]}'));
assert.equal(result.client_pid,50);assert.equal(result.pointer,true);assert.equal(result.keyboard,true);
picked=foreign;assert.equal(JSON.parse(w.Selector('{"point":[40,50]}')).pointer,false);
request.selector=foreign;assert.deepEqual(JSON.parse(w.Selector('{}')),{});
request.selector=selector;owner=':1.99';assert.deepEqual(JSON.parse(w.Selector('{}')),{});
owner=request.sender;fake.Main.sessionMode.isLocked=true;assert.deepEqual(JSON.parse(w.Selector('{}')),{});
console.log('GNOME owned selector reference, sender and occlusion refusals passed');
