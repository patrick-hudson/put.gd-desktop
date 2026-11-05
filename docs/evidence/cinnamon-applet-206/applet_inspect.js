(function(){
 const AM = imports.ui.appletManager;
 const list = AM.getRunningInstancesForUuid('notifications@cinnamon.org');
 if (!list || !list.length) return JSON.stringify({instances: 0});
 const a = list[0];
 const box = actor => { if (!actor) return null; const [x, y] = actor.get_transformed_position(); const [w, h] = actor.get_transformed_size();
   return {x: Math.round(x), y: Math.round(y), w: Math.round(w), h: Math.round(h), mapped: actor.mapped}; };
 // Only put.gd's notifications are described; others are only counted.
 const ours = [], all = a.notifications || [];
 for (const n of all) {
  const source = n.source ? n.source.title : '';
  if (source !== 'put.gd' && !(n.title || '').startsWith('put.gd')) continue;
  const buttons = n._actionArea ? n._actionArea.get_children().map(b => ({label: b.label, box: box(b)})) : [];
  ours.push({title: n.title, source: source, resident: n.resident, box: box(n.actor), buttons: buttons});
 }
 return JSON.stringify({instances: list.length, total: all.length, others: all.length - ours.length,
  applet: box(a.actor), menuOpen: a.menu ? a.menu.isOpen : null, menu: a.menu ? box(a.menu.actor) : null, ours: ours});
})()
