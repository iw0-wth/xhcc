"""Standalone type-graph classifier, not native aggregate ABI interoperability.
Builds only a small Wasm harness. Expected class vectors are independent subset
policy vectors, not claimed GCC/Clang oracle results.
"""
from pathlib import Path
import os,sys,subprocess,unittest
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/typeclass-tests'
SDK=Path(os.environ.get('WASI_SDK_PATH',r'D:\wasi-sdk-34.0-x86_64-windows'));WASM=os.environ.get('WASMTIME',r'C:\Program Files\Wasmtime\bin\wasmtime.exe')
HARNESS=r'''
#include "xh_typeclass.h"
#include <stdio.h>
#include <stdlib.h>
int fs_file_size(const char*p){FILE*f=fopen(p,"rb");long n;if(!f)return -1;fseek(f,0,SEEK_END);n=ftell(f);fclose(f);return(int)n;}
int fs_read_file(const char*p,char*b,int cap){FILE*f=fopen(p,"rb");int n;if(!f)return -1;n=(int)fread(b,1,cap-1,f);b[n]=0;fclose(f);return n;}
void diag_tool(int s,const char*w,const char*m,const char*a,const char*b,const char*c,const char*d,const char*e,const char*f){fprintf(stderr,"%s: %s\n",w,m);}
int main(int argc,char**argv){XhTypeclassNode nodes[128];XhTypeclassEdge edges[256];XhTypeclassGraph graph;XhTypeclassResult out;unsigned n,e,i;int k=4,r;if(argc<4)return 2;n=atoi(argv[2]);e=atoi(argv[3]);if(n>128||e>256||argc!=4+6*n+2*e)return 2;
for(i=0;i<n;i++){nodes[i].form=atoi(argv[k++]);nodes[i].kind=atoi(argv[k++]);nodes[i].size=atoi(argv[k++]);nodes[i].alignment=atoi(argv[k++]);nodes[i].first_edge=atoi(argv[k++]);nodes[i].edge_count=atoi(argv[k++]);}for(i=0;i<e;i++){edges[i].node=atoi(argv[k++]);edges[i].offset=atoi(argv[k++]);}graph.nodes=nodes;graph.node_count=n;graph.edges=edges;graph.edge_count=e;if(xh_typeclass_set_path("x86_64",argv[1]))return 2;r=xh_typeclass_select("x86_64.aggregate","typeclass",&graph,0,&out);printf("%d %d %u",r,out.memory,out.count);for(i=0;i<out.count;i++)printf(" %u %u %d",out.fragments[i].offset,out.fragments[i].size,out.fragments[i].class_id);puts("");return 0;}
'''
def leaf(kind,size,align):return (1,kind,size,align,0,0)
def group(size,align,count,first=0):return (0,0,size,align,first,count)
class Typeclass(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  OUT.mkdir(parents=True,exist_ok=True);(OUT/'harness.c').write_text(HARNESS);env=os.environ.copy();pd=Path(sys.executable).parent;env['PATH']=os.pathsep.join([str(pd),str(pd/'Library/bin'),env.get('PATH','')])
  p=subprocess.run([str(SDK/'bin/clang.exe'),'--target=wasm32-wasip1','-O2','-Iinclude','-include','include/xh_small.h','build/typeclass-tests/harness.c','src/backend/rules/typeclass.c','src/backend/rules/template.c','-o','build/typeclass-tests/vm.wasm'],cwd=ROOT,env=env,capture_output=True,text=True)
  if p.returncode:raise RuntimeError(p.stderr)
 def run_graph(self,nodes,edges=(),path='packages/x86_64/aggregate.rules'):
  p=subprocess.run([WASM,'run','--dir=.','build/typeclass-tests/vm.wasm',str(path),str(len(nodes)),str(len(edges)),*[str(x) for row in [*nodes,*edges] for x in row]],cwd=ROOT,capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stderr);v=list(map(int,p.stdout.split()));return v[:3],[v[i:i+3] for i in range(3,len(v),3)]
 def test_fifteen_scalar_aggregate_vectors(self):
  vectors=[([leaf(1,8,8)],[],[1]),([leaf(2,4,4)],[],[2]),([leaf(3,8,8)],[],[2]),([leaf(4,8,8)],[],[1]),
   ([group(16,8,2),leaf(1,8,8),leaf(1,8,8)],[(1,0),(2,8)],[1,1]),
   ([group(16,8,2),leaf(3,8,8),leaf(3,8,8)],[(1,0),(2,8)],[2,2]),
   ([group(16,8,2),leaf(3,8,8),leaf(1,8,8)],[(1,0),(2,8)],[2,1]),
   ([group(16,8,2),leaf(1,8,8),leaf(3,8,8)],[(1,0),(2,8)],[1,2]),
   ([group(12,4,3),leaf(2,4,4)],[(1,0),(1,4),(1,8)],[2,2]),
   ([group(8,8,2),leaf(3,8,8),leaf(1,8,8)],[(1,0),(2,0)],[1]),
   ([group(8,4,2),leaf(2,4,4),leaf(1,4,4)],[(1,0),(2,4)],[1]),
   ([group(16,8,2),group(8,8,1,2),leaf(3,8,8),leaf(1,8,8)],[(1,0),(3,8),(2,0)],[2,1]),
   ([group(16,8,2),leaf(3,8,8)],[(1,0),(1,8)],[2,2]),
   ([group(16,8,1),leaf(1,1,1)],[(1,9)],[0,1]),
   ([group(0,1,0)],[],[])]
  for nodes,edges,want in vectors:
   with self.subTest(nodes=nodes,edges=edges):
    head,fragments=self.run_graph(nodes,edges);self.assertEqual(head,[0,0,len(want)]);self.assertEqual([x[2] for x in fragments],want)
 def test_memory_misalignment_size_and_union(self):
  for nodes,edges in [([group(9,1,2),leaf(1,1,1),leaf(3,8,8)],[(1,0),(2,1)]),([group(8,1,1),leaf(3,8,8)],[(1,0)]),([group(24,8,1),leaf(1,8,8)],[(1,0)]),([group(16,8,1),leaf(3,8,8)],[(1,4)])]:
   self.assertEqual(self.run_graph(nodes,edges),([0,1,0],[]))
 def test_graph_failures_and_unknown_types(self):
  bad=[([group(8,8,1)],[(0,0)]),([group(8,8,1),leaf(1,8,8)],[(1,1)]),([leaf(99,8,8)],[]),([leaf(1,8,3)],[]),([group(8,8,1)],[(99,0)]),([group(24,8,1)],[(0,0)]),([group(24,8,1),leaf(99,24,8)],[(1,0)])]
  for n,e in bad:self.assertEqual(self.run_graph(n,e),([-1,0,0],[]))
  nodes=[group(8,8,1,i) for i in range(33)]+[leaf(1,8,8)];edges=[(i+1,0) for i in range(33)];self.assertEqual(self.run_graph(nodes,edges),([-1,0,0],[]))
 def test_external_merge_and_cleanup_changes(self):
  base=(ROOT/'packages/x86_64/aggregate.rules').read_text();path=OUT/'variant.rules';nodes=[group(8,8,2),leaf(3,8,8),leaf(1,8,8)];edges=[(1,0),(2,0)]
  path.write_text(base.replace('text 2 1 1\n','text 2 1 2\n'));self.assertEqual(self.run_graph(nodes,edges,path.relative_to(ROOT))[1],[[0,8,2]])
  path.write_text(base.replace('text 1 0 0 0\n','text 1 0 1 2\n'));self.assertEqual(self.run_graph([group(9,1,1),leaf(3,8,8)],[(1,1)],path.relative_to(ROOT))[0],[0,0,2])
  for changed in [base.replace('text 2 1 1\n','text 2 1 9\n'),base.replace('rule typeclass.merge.7\n','rule missing.merge.7\n'),base.replace('text 2 1 1\n','text 1 1 1\n')]:
   path.write_text(changed);self.assertEqual(self.run_graph(nodes,edges,path.relative_to(ROOT)),([-1,0,0],[]))
if __name__=='__main__':unittest.main()
