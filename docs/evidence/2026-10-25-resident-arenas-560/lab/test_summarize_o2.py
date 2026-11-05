"""Tests for summarize_o2.settle_window with 30 s, 300 s and partial/missing settle data."""
import importlib.util, sys
spec=importlib.util.spec_from_file_location('s',sys.argv[1]);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
def rows(seconds,step=5,start=100.0,peak_at=None):
    r=[{'label':'drained','elapsedSeconds':start,'family':{'totals':{'rssBytes':10,'pssBytes':5}}}]
    for i in range(1,seconds//step+1):
        v=99 if peak_at==i*step else 20
        r.append({'label':f'settle-{i*step}','elapsedSeconds':start+i*step+0.3,'family':{'totals':{'rssBytes':v,'pssBytes':v}}})
    return r
w=m.settle_window(rows(30));assert 'last270MaxFamily' not in w and w['settleObservedSeconds']==30.3,w
w=m.settle_window(rows(300));assert w['last270MaxFamily']=={'rssBytes':20,'pssBytes':20} and w['last270Samples']==55,w
w=m.settle_window(rows(300,peak_at=25));assert w['last270MaxFamily']['rssBytes']==20,w   # before the final 270 s window
w=m.settle_window(rows(300,peak_at=40));assert w['last270MaxFamily']['rssBytes']==99,w   # inside it
w=m.settle_window(rows(295));assert 'last270MaxFamily' not in w,w                         # partial settle
w=m.settle_window([{'label':'settle-5','elapsedSeconds':1,'family':{'totals':{'rssBytes':1,'pssBytes':1}}}]);assert w=={'settleObservedSeconds':0.0},w
print('summarize_o2 settle_window: 6 cases pass')
