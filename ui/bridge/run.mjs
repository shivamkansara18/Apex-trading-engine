import {spawn} from 'node:child_process';
import {createInterface} from 'node:readline';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
import {mkdirSync, writeFileSync, renameSync} from 'node:fs';
import {startBridge} from './server.mjs';
import {symbolName} from './markets.mjs';
const root = fileURLToPath(new URL('../../', import.meta.url));
const args = process.argv.slice(2);
if (args.length === 1 && args[0] === '--help') {
  console.log('Usage: npm start -- [--symbols APEX,ALPHA,BETA] [--gateway-port 9000] [--port 8080] [--build-dir build/debug] [--journal NEW_FILE | --journal-dir NEW_DIRECTORY]');
  process.exit(0);
}
const known = new Set(['--symbols','--gateway-port','--port','--build-dir','--journal','--journal-dir']);
if (args.length % 2 || args.some((value,i) => i % 2 === 0 && !known.has(value))) throw new Error('Invalid options; use --help');
function option(name, fallback) { const i=args.indexOf(name); return i<0 ? fallback : args[i+1]; }
function port(name, fallback) {
  const value=Number(option(name,fallback));
  if (!Number.isInteger(value)||value<0||value>65535) throw new Error(`Invalid ${name}`); return value;
}
const symbols=option('--symbols','APEX').split(',').map(symbolName);
if (!symbols.length||symbols.length>8||new Set(symbols).size!==symbols.length) throw new Error('Use 1-8 unique symbols');
if (args.includes('--journal')&&(symbols.length>1||args.includes('--journal-dir'))) throw new Error('Use --journal-dir for multiple symbols; journal options are mutually exclusive');
const basePort=port('--gateway-port',9000), httpPort=port('--port',8080);
const executable=path.resolve(root,option('--build-dir','build/debug'),'apex_gateway');
const partitions=[];
const journalDirectory=args.includes('--journal-dir') ? path.resolve(option('--journal-dir')) : null;
if (journalDirectory) { mkdirSync(path.dirname(journalDirectory),{recursive:true}); mkdirSync(journalDirectory); }
let bridge, stopping=false;
const createdAt=new Date().toISOString();
function manifest(initial=false) {
  if (!journalDirectory) return;
  const file=path.join(journalDirectory,'session.json');
  const text=JSON.stringify({schemaVersion:1,routingVersion:2,tcpVersion:1,createdAt,stopping,
    markets:partitions.map(({route,child,journal})=>({...route,pid:child.pid,journal:journal?path.basename(journal):null,exitCode:child.exitCode,signal:child.signalCode}))},null,2)+'\n';
  if (initial) writeFileSync(file,text,{flag:'wx'});
  else { writeFileSync(file+'.tmp',text); renameSync(file+'.tmp',file); }
}
async function stop() {
  if (stopping) return; stopping=true;
  try { if (bridge) await bridge.close(); }
  finally {
    await Promise.all(partitions.map(async ({child})=>{
      if (child.exitCode!==null||child.signalCode!==null) return;
      const exit=new Promise(resolve=>child.once('exit',resolve));
      child.kill('SIGINT'); const timer=setTimeout(()=>child.kill('SIGKILL'),5000);
      await exit; clearTimeout(timer);
    }));
    manifest();
  }
}
for (const signal of ['SIGINT','SIGTERM']) process.once(signal,()=>stop().catch(error=>{console.error(error.message);process.exitCode=1;}));
async function launch(symbol,index) {
  const journal=journalDirectory ? path.join(journalDirectory,symbol+'.apex') : args.includes('--journal') ? path.resolve(option('--journal')) : null;
  const gatewayArgs=['--port',String(index===0 ? basePort : 0)]; if(journal) gatewayArgs.push('--journal',journal);
  const child=spawn(executable,gatewayArgs,{stdio:['ignore','pipe','pipe']});
  const route={symbol,gatewayPort:0,available:false}; partitions.push({child,route,journal});
  child.stderr.on('data',bytes=>process.stderr.write(symbols.length>1 ? `[${symbol}] ${bytes}` : bytes));
  child.once('exit',()=>{
    route.available=false;
    if (!stopping&&bridge) {
      console.error(`Partition ${symbol} exited; its book is unavailable and will not be restarted automatically.`);
      if (symbols.length===1) { process.exitCode=1; stop().catch(error=>console.error(error.message)); }
      else {
        try { manifest(); } catch(error) {
          console.error('Session manifest failed: '+error.message); process.exitCode=1;
          stop().catch(failure=>console.error(failure.message));
        }
      }
    }
  });
  const lines=createInterface({input:child.stdout});
  await new Promise((resolve,reject)=>{
    const timer=setTimeout(()=>reject(new Error(`${symbol}: gateway startup timed out`)),5000);
    const fail=error=>{clearTimeout(timer);reject(error);};
    child.once('error',fail); child.once('exit',()=>fail(new Error(`${symbol}: gateway exited before ready`)));
    lines.on('line',line=>{
      console.log(symbols.length>1 ? `[${symbol}] ${line}` : line);
      const match=line.match(/listening on 127\.0\.0\.1:(\d+)/);
      if(match){clearTimeout(timer);route.gatewayPort=Number(match[1]);route.available=true;resolve();}
    });
  });
}
try {
  if(journalDirectory) manifest(true);
  for(let i=0;i<symbols.length&&!stopping;i++) await launch(symbols[i],i);
  if(!stopping){
    if(partitions.some(p=>!p.route.available)) throw new Error('A partition failed during startup');
    bridge=await startBridge({port:httpPort,markets:partitions.map(p=>p.route)});
    if(stopping) await bridge.close();
    else {
      if(partitions.some(p=>!p.route.available)) throw new Error('A partition exited before bridge startup completed');
      manifest(); console.log(`Apex dashboard: http://127.0.0.1:${bridge.port}`);
      for(const {route} of partitions) console.log(`Market ${route.symbol}: 127.0.0.1:${route.gatewayPort}`);
      console.log('Adaptive activity: npm --prefix ui run demo -- --symbol '+symbols[0]+' --bridge-port '+bridge.port);
    }
  }
} catch(error) {
  if(!stopping){console.error(error.message);process.exitCode=1;} await stop();
}
