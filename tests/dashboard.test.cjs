const fs=require('fs'),vm=require('vm'),assert=require('assert');
const source=fs.readFileSync('Balisto_Live_BMS_Dashboard_Names.ino','utf8');
const js=source.split('<script>')[1].split('</script>')[0];
function element(){return {textContent:'',className:'',style:{},children:[],classList:{add(){}},append(...x){this.children.push(...x)},replaceChildren(...x){this.children=x}}}
async function run(fetcher){
 const nodes=Object.fromEntries(['refresh','pause','control','connection','discovered','successful','unavailable','stage','list'].map(id=>[id,element()]));
 let pending=0,maxPending=0,requests=[],scheduled=[];
 const context={document:{getElementById:id=>nodes[id],createElement:element},AbortController,Map,Error,
  setTimeout:(fn,ms)=>{scheduled.push(ms);return 1},clearTimeout:()=>{},fetch:async(url,options)=>{requests.push(url);pending++;maxPending=Math.max(pending,maxPending);try{return await fetcher(url,options)}finally{pending--}}};
 await vm.runInNewContext(js,context);
 assert.equal(maxPending,1,'Requests must be sequential');
 assert.equal(scheduled.at(-1),300000,'Automatic refresh must wait five minutes');
 return {nodes,requests};
}
function battery(i){return {mac:'12:34:5b:00:00:'+i,name:'SP16S60A'+i,soc:42,voltage:52.32,cycles:16,rssi:-50,hasReading:true,stale:false,status:'OK',seenAgeSec:1,readAgeSec:1}}
function ok(data){return {ok:true,status:200,text:async()=>JSON.stringify(data)}}
(async()=>{
 const good=await run(async url=>{const offset=Number(url.split('=')[1]);return ok({firmware:'BMS-2026-10-16-R7',total:93,stage:'Reading',scanNumber:1,next:offset+8<93?offset+8:-1,batteries:Array.from({length:Math.min(8,93-offset)},(_,n)=>battery(offset+n))})});
 assert.equal(good.nodes.list.children.length,93);assert.equal(good.nodes.successful.textContent,93);assert.equal(good.requests.length,12);
 const empty=await run(async()=>ok({firmware:'BMS-2026-10-16-R7',total:0,stage:'Scanning',scanNumber:1,next:-1,batteries:[]}));assert.equal(empty.nodes.list.children.length,0);
 const invalid=await run(async()=>({ok:true,text:async()=>'{broken'}));assert.match(invalid.nodes.stage.textContent,/Invalid JSON/);
 const partial=await run(async url=>{if(url.endsWith('=8'))throw new Error('Failed to fetch');return ok({firmware:'BMS-2026-10-16-R7',total:93,stage:'Reading',scanNumber:1,next:8,batteries:Array.from({length:8},(_,n)=>battery(n))})});assert.equal(partial.nodes.list.children.length,8);assert.equal(partial.nodes.connection.textContent,'PARTIAL DATA; RETRYING');
 const timeout=await run(async()=>{const e=new Error('aborted');e.name='AbortError';throw e});assert.match(timeout.nodes.stage.textContent,/network timeout/);
 const mismatch=await run(async()=>ok({firmware:'old'}));assert.match(mismatch.nodes.stage.textContent,/Firmware mismatch/);
 console.log('PASS: 93-battery pagination, empty list, invalid JSON, partial transfer, timeout, version mismatch; no overlapping requests.');
})().catch(e=>{console.error(e);process.exit(1)});
