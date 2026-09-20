/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * Lado de conteudo da ponte de midia imersiva do FxR.
 *
 * Por que este ator existe: o botao de projecao vive no chrome (fxrui.js) e o
 * <video> vive na pagina, noutro processo -- o browser do fxrui e
 * remote="true". O chrome nao alcanca o elemento. Este ator e a entrega:
 * recebe a ordem do chrome e executa o renderer DENTRO da pagina, onde o
 * video esta.
 *
 * ESTADO: recuperacao de sessao e VR180 por metadados. Sincronizacao 1B
 * e controles 1C preservados. Caminho mesh separado do 360 ERP/EAC.
 *
 * O renderer roda num sandbox com `sandboxPrototype` = janela de conteudo e
 * `wantXrays: false`, e o sandbox recebe a JANELA, nao [principal] -- um array
 * cria principal expandido, que enxerga a pagina por Xray e faz todo quadro
 * morrer em "Accessing TypedArray data over Xrays is forbidden" (ADR-31).
 *
 * O shader e o validado no headset (ADR-30): atlas EAC 3x2 do YouTube,
 * ceu/chao a 90 graus espelhados, atras a 270 nao espelhado.
 */

// Avaliado no compartimento da pagina; publica __fxrControl no sandbox para
// que o chrome possa pedir inicio e saida.
const MESH_SOURCE = String.raw`/* MPL-2.0. Parser for Google Spherical Video V2 (mshp/legacy ytmp).
 * Format: https://github.com/google/spatial-media/blob/master/docs/spherical-video-v2-rfc.md
 * Independent implementation with bounded allocation and strict indices.
 */
var FxRMesh = (() => {
  const MAX_INIT = 2 * 1024 * 1024, MAX_RAW = 2 * 1024 * 1024;
  const fail = message => { throw new Error('VR mesh: ' + message); };
  const four = (b,p) => String.fromCharCode(...b.subarray(p,p+4));
  function view(b) { return new DataView(b.buffer,b.byteOffset,b.byteLength); }
  function crc32(b) {
    let crc = -1;
    for (const byte of b) {
      crc ^= byte;
      for (let bit=0;bit<8;bit++) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
    }
    return (crc ^ -1) >>> 0;
  }
  function boxes(b,start,end,fn) {
    const d=view(b);
    for(let p=start;p<end;) {
      if(p+8>end) fail('truncated box');
      let size=d.getUint32(p), header=8;
      if(size===1) {
        if(p+16>end || d.getUint32(p+8)!==0) fail('oversized box');
        size=d.getUint32(p+12); header=16;
      }
      if(size===0) size=end-p;
      if(size<header || p+size>end) fail('invalid box length');
      fn(four(b,p+4),p+header,p+size);
      p+=size;
    }
  }
  function mp4(b) {
    const tracks=[], d=view(b);
    const containers=new Set(['moov','mdia','minf','stbl','sv3d','proj']);
    const visual=new Set(['avc1','avc3','hvc1','hev1','vp09','av01','encv']);
    function walk(start,end,track,depth) {
      if(depth>12) fail('nested boxes');
      boxes(b,start,end,(type,p,e)=>{
        if(type==='trak') { const t={container:'mp4',pose:[0,0,0],stereo:0};walk(p,e,t,depth+1);if(t.projection)tracks.push(t); }
        else if(containers.has(type)) walk(p,e,track,depth+1);
        else if(type==='stsd') { if(p+8>e)fail('short stsd');walk(p+8,e,track,depth+1); }
        else if(visual.has(type) && track) {
          if(p+78>e) fail('short visual sample entry');
          track.width=d.getUint16(p+24);track.height=d.getUint16(p+26);
          walk(p+78,e,track,depth+1);
        } else if(track && type==='st3d') {
          if(e-p!==5 || b[p]!==0 || b[p+4]>4)fail('stereo mode');
          track.stereo=b[p+4];
        } else if(track && type==='prhd') {
          if(e-p!==16 || b[p]!==0)fail('projection pose');
          track.pose=[4,8,12].map(o=>d.getInt32(p+o)/65536);
        } else if(track && (type==='mshp'||type==='ytmp')) {
          if(track.projection)fail('multiple projections');
          track.projection=b.slice(p,e);track.box=type;
        }
      });
    }
    walk(0,b.length,null,0);return tracks;
  }
  function webm(b) {
    const tracks=[];
    function vint(p,id) {
      if(p>=b.length || b[p]===0)fail('EBML integer');
      let n=1,mask=128;while(!(b[p]&mask)){n++;mask>>=1;}
      if(n>(id?4:8) || p+n>b.length)fail('EBML integer length');
      let v=id?b[p]:b[p]&(mask-1), unknown=!id && v===mask-1;
      for(let j=1;j<n;j++){v=v*256+b[p+j];unknown=unknown&&b[p+j]===255;}
      return {v,n,unknown};
    }
    function uint(p,e) { if(e-p>6)fail('EBML uint');let n=0;while(p<e)n=n*256+b[p++];return n; }
    function walk(p,end,t,depth) {
      if(depth>10)fail('nested EBML');
      while(p<end) {
        const id=vint(p,true);p+=id.n;const size=vint(p,false);p+=size.n;
        const e=size.unknown?end:p+size.v;
        if(e>end || e<p)fail('EBML length');
        if(id.v===0x1f43b675)break; // Cluster contains media, never inspect it.
        if(id.v===0xae) {const next={container:'webm',pose:[0,0,0],stereo:0};walk(p,e,next,depth+1);if(next.projection)tracks.push(next);}
        else if([0x18538067,0x1654ae6b,0xe0,0x7670].includes(id.v))walk(p,e,t,depth+1);
        else if(t) {
          if(id.v===0xb0)t.width=uint(p,e);
          else if(id.v===0xba)t.height=uint(p,e);
          else if(id.v===0x7672)t.projection=b.slice(p,e);
          else if(id.v===0x7671)t.projectionType=uint(p,e);
          else if(id.v===0x53b8){const modes={0:0,1:2,3:1,11:4};t.stereo=modes[uint(p,e)]??-1;}
          else if([0x7673,0x7674,0x7675].includes(id.v)) {
            if(e-p!==4&&e-p!==8)fail('EBML float');
            t.pose[id.v-0x7673]=e-p===4?view(b).getFloat32(p):view(b).getFloat64(p);
          }
        }
        p=e;
      }
    }
    walk(0,b.length,null,0);return tracks.filter(t=>t.projectionType===3);
  }
  function extract(input) {
    const b=input instanceof Uint8Array?input:new Uint8Array(input);
    if(b.length>MAX_INIT || b.length<8)fail('initialization size');
    return b[0]===0x1a&&b[1]===0x45?webm(b):mp4(b);
  }
  async function inflate(b) {
    const stream=new Blob([b]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
    const reader=stream.getReader();const chunks=[];let total=0;
    try {
      while(true) {
        const {value,done}=await reader.read();if(done)break;
        total+=value.length;if(total>MAX_RAW)fail('decompressed size');chunks.push(value);
      }
    } finally {await reader.cancel().catch(()=>{});reader.releaseLock();}
    const result=new Uint8Array(total);let p=0;for(const c of chunks){result.set(c,p);p+=c.length;}return result;
  }
  function rawMeshes(b) {
    if(b.length>MAX_RAW)fail('mesh size');
    const meshes=[];
    boxes(b,0,b.length,(type,start,end)=>{
      if(type!=='mesh')return;
      if(meshes.length>=2)fail('more than two eyes');
      let p=start, bit=0;const d=view(b);
      const u32=()=>{if(p+4>end)fail('truncated integer');const x=d.getUint32(p);p+=4;return x;};
      const count=u32();if(count<1||count>10000||p+4*count>end)fail('coordinate count');
      const coords=new Float32Array(count);
      for(let i=0;i<count;i++,p+=4){coords[i]=d.getFloat32(p);if(!Number.isFinite(coords[i]))fail('nonfinite coordinate');}
      const vertices=u32();if(vertices<1||vertices>32000)fail('vertex count');
      bit=p*8;
      function bits(n) {if(bit+n>end*8)fail('truncated bitstream');let v=0;while(n--){v=v*2+((b[bit>>3]>>(7-(bit&7)))&1);bit++;}return v;}
      const zigzag=n=>(n>>>1)^-(n&1);
      const attrs=new Float32Array(vertices*5), prev=[0,0,0,0,0];
      for(let i=0;i<vertices;i++)for(let a=0;a<5;a++){
        const index=prev[a]+zigzag(bits(Math.ceil(Math.log2(2*count))));
        if(index<0||index>=count)fail('coordinate index');attrs[i*5+a]=coords[index];prev[a]=index;
      }
      for(let i=0;i<vertices;i++) {
        if(Math.hypot(...attrs.subarray(i*5,i*5+3))<1e-6)fail('zero direction');
      }
      bit=Math.ceil(bit/8)*8;
      const lists=bits(32);if(lists<1||lists>32)fail('submesh count');
      const submeshes=[];let total=0;
      for(let k=0;k<lists;k++) {
        const texture=bits(8),mode=bits(8),n=bits(32);total+=n;
        if(texture!==0||mode>2||n<3||total>128000)fail('unsupported submesh');
        if(mode===0&&n%3)fail('triangle count');
        const data=new Float32Array(n*5);let index=0;
        for(let i=0;i<n;i++) {
          index+=zigzag(bits(Math.ceil(Math.log2(2*vertices))));
          if(index<0||index>=vertices)fail('vertex index');
          data.set(attrs.subarray(index*5,index*5+5),i*5);
        }
        submeshes.push({mode,count:n,data});
      }
      meshes.push(submeshes);
    });
    if(!meshes.length)fail('no meshes');return meshes;
  }
  async function decode(track) {
    const b=track.projection;
    if(b.length<12||b.length>MAX_INIT||b[0]!==0)fail('mesh header');
    const expected=view(b).getUint32(4);
    if(expected!==crc32(b.subarray(8)))fail('mesh CRC');
    const format=four(b,8);
    const raw=format==='raw '?b.subarray(12):format==='dfl8'?await inflate(b.subarray(12)):fail('compression');
    const meshes=rawMeshes(raw);
    if(track.stereo<0||track.stereo>4)fail('stereo layout');
    if(meshes.length===1&&track.stereo===3)fail('custom stereo without two meshes');
    if(track.pose.some(x=>!Number.isFinite(x)))fail('pose');
    return {...track,projection:undefined,meshes};
  }
  return {extract,decode,rawMeshes,crc32,MAX_INIT};
})();
if(typeof module==='object')module.exports=FxRMesh;

/* Observe only initialization metadata, never change the bytes passed to MSE. */
function createFxRMeshObserver(report) {
  const states=new WeakMap(), urls=new Map(), originals=[];
  let stopped=false, stamp=0;
  const identity=()=>location.hostname.includes('youtube.com')
    ? new URL(location.href).searchParams.get('v') : location.href;
  function replace(obj,key,fn) {
    const original=obj[key];obj[key]=fn(original);
    originals.push(()=>{if(obj[key]===wrapped)obj[key]=original;});
    const wrapped=obj[key];
  }
  async function inspect(state, bytes, token, id) {
    try {
      const tracks=FxRMesh.extract(bytes);
      const decoded=await Promise.all(tracks.map(FxRMesh.decode));
      if(stopped||token!==state.token||id!==identity())return;
      state.tracks=decoded;state.id=id;state.stamp=++stamp;
      for(const t of decoded)report('malha recebida: '+t.width+'x'+t.height+
        ', '+t.meshes.length+' malha(s), stereo='+t.stereo+', '+t.box);
    } catch(error) {
      if(token===state.token){state.tracks=[];report('metadados de malha recusados: '+error.message);}
    }
  }
  if(typeof MediaSource==='undefined'||typeof SourceBuffer==='undefined')
    return {get:()=>null,stop(){},summary:()=>[]};
  replace(URL,'createObjectURL',original=>function(source) {
    const result=Reflect.apply(original,this,arguments);
    if(source instanceof MediaSource) {
      urls.set(result,source);
      if(urls.size>8)urls.delete(urls.keys().next().value);
    }
    return result;
  });
  replace(MediaSource.prototype,'addSourceBuffer',original=>function(type) {
    const buffer=Reflect.apply(original,this,arguments);
    if(String(type).startsWith('video/'))states.set(buffer,{token:0,tracks:[],id:null,stamp:0});
    return buffer;
  });
  replace(SourceBuffer.prototype,'appendBuffer',original=>function(value) {
    const result=Reflect.apply(original,this,arguments);
    try {
      const state=states.get(this);
      if(stopped||!state)return result;
      const b=ArrayBuffer.isView(value)?new Uint8Array(value.buffer,value.byteOffset,value.byteLength):new Uint8Array(value);
      const type=b.length>=8?String.fromCharCode(...b.subarray(4,8)):'';
      const init=type==='ftyp'||type==='moov'||(b[0]===0x1a&&b[1]===0x45&&b[2]===0xdf&&b[3]===0xa3);
      if(init) {
        state.token++;state.tracks=[];state.id=null;
        if(b.length<=FxRMesh.MAX_INIT)inspect(state,b.slice(),state.token,identity());
        else report('segmento inicial maior que o limite de metadados');
      }
    } catch(error) {report('observador MSE: '+error.message);}
    return result;
  });
  function candidates(video) {
    const source=urls.get(video.currentSrc);
    if(!source)return [];
    const all=[];
    for(const buffer of source.sourceBuffers) {
      const state=states.get(buffer);
      if(state?.id===identity())for(const track of state.tracks) {
        if(track.width===video.videoWidth&&track.height===video.videoHeight)
          all.push({track,stamp:state.stamp});
      }
    }
    return all.sort((a,b)=>b.stamp-a.stamp);
  }
  return {
    get(video) {return candidates(video)[0]?.track??null;},
    summary(video) {return candidates(video).map(({track:t})=>({width:t.width,height:t.height,stereo:t.stereo,eyes:t.meshes.length,counts:t.meshes.map(m=>m.map(s=>s.count))}));},
    stop() {stopped=true;for(const undo of originals.reverse())undo();urls.clear();},
  };
}
if(typeof module==='object')module.exports=createFxRMeshObserver;
`;

const RENDERER_SOURCE = String.raw`
var __fxrControl = (function () {
  const VS = [
    "attribute vec3 aPos;",
    "uniform mat4 uProj, uView;",
    "varying vec3 vDir;",
    "void main() { vDir = aPos; gl_Position = uProj * uView * vec4(aPos,1.0); }",
  ].join("\n");

  // Amostragem equi-angular: o YouTube distribui as amostras por angulo, nao
  // linearmente pela face. Por isso o atan e o passo 4/PI.
  const FS = [
    "precision highp float;",
    "varying vec3 vDir;",
    "uniform sampler2D uTex;",
    // Giro em torno do eixo vertical, para recentralizar a visao.
    "uniform float uYaw;",
    // Etapa 2A: 0 = atlas EAC do YouTube (projectionType MESH, ADR-30);
    // 1 = equirretangular (projectionType EQUIRECTANGULAR). Escolhido pelo
    // sinal que o proprio YouTube declara, nunca pela proporcao da imagem.
    "uniform float uKind;",
    "const float PI = 3.141592653589793;",
    "vec2 dirToEAC(vec3 d) {",
    "  vec3 a = abs(d);",
    "  vec2 c; float col, row, rot = 0.0, which = -1.0;",
    "  if (a.z >= a.x && a.z >= a.y && d.z < 0.0) { c = vec2(d.x,-d.y)/a.z; col=1.0; row=0.0; }",
    "  else if (a.x >= a.y && a.x >= a.z && d.x < 0.0) { c = vec2(-d.z,-d.y)/a.x; col=0.0; row=0.0; }",
    "  else if (a.x >= a.y && a.x >= a.z && d.x > 0.0) { c = vec2(d.z,-d.y)/a.x; col=2.0; row=0.0; }",
    "  else if (a.y >= a.x && a.y >= a.z && d.y > 0.0) { c = vec2(d.x,d.z)/a.y; col=2.0; row=1.0; rot=1.0; which=0.0; }",
    "  else if (a.z >= a.x && a.z >= a.y && d.z > 0.0) { c = vec2(-d.x,-d.y)/a.z; col=1.0; row=1.0; rot=1.0; which=1.0; }",
    "  else { c = vec2(d.x,-d.z)/a.y; col=0.0; row=1.0; rot=1.0; which=2.0; }",
    "  vec2 e = atan(c) * (4.0/PI);",
    "  e = clamp(e,-1.0,1.0) * 0.5 + 0.5;",
    "  if (rot > 0.5) {",
    "    if (which > 0.5 && which < 1.5) { e = vec2(1.0-e.y, e.x); }",
    "    else { e = vec2(e.y, 1.0-e.x); e.x = 1.0-e.x; }",
    "  }",
    "  return vec2((col+e.x)/3.0, (row+e.y)/2.0);",
    "}",
    // Etapa 2A. Longitude/latitude na mesma convencao da esfera de sphere():
    // y = cos(phi) e th = atan(x, -z), com a primeira linha da textura no ceu.
    // Validado no lote 2B; preservar a convencao ao adicionar outros formatos.
    "vec2 dirToERP(vec3 d) {",
    "  float th = atan(d.x, -d.z);",
    "  float v = acos(clamp(d.y, -1.0, 1.0)) / PI;",
    "  return vec2((th + PI) / (2.0 * PI), v);",
    "}",

    "void main() {",
    "  vec3 d = normalize(vDir);",
    // R_y(uYaw). Com uYaw = -yaw_da_cabeca no instante do clique, a direcao
    // olhada passa a amostrar a frente da textura: R_y(-t) aplicado a
    // (-sin t, 0, -cos t) da (0, 0, -1).
    "  float cy = cos(uYaw), sy = sin(uYaw);",
    "  d = vec3(cy * d.x + sy * d.z, d.y, -sy * d.x + cy * d.z);",
    "  vec2 uv = uKind > 0.5 ? dirToERP(d) : dirToEAC(d);",
    "  gl_FragColor = texture2D(uTex, uv);",
    "}",
  ].join("\n");

  const meshMetadata = createFxRMeshObserver(report);
  let active = null;
  let fallbackBtn = null;

  // O relato sobe por evento DOM, nao por callback recebido do ator. Uma
  // funcao de chrome entregue a codigo com principal da pagina nao e
  // chamavel do outro lado sem Cu.exportFunction; o evento evita a travessia
  // por completo -- o ator escuta e repassa.
  function report(text) {
    window.dispatchEvent(
      new CustomEvent("fxr-media-status", { detail: String(text) })
    );
  }

  function sphere() {
    const su = 64, sv = 32, pos = [], idx = [];
    for (let v = 0; v <= sv; v++) {
      const phi = (v / sv) * Math.PI;
      for (let u = 0; u <= su; u++) {
        const th = -Math.PI + (u / su) * 2 * Math.PI;
        pos.push(Math.sin(phi) * Math.sin(th) * 10,
                 Math.cos(phi) * 10,
                 -Math.sin(phi) * Math.cos(th) * 10);
      }
    }
    for (let v = 0; v < sv; v++) {
      for (let u = 0; u < su; u++) {
        const a = v * (su + 1) + u, b = a + su + 1;
        idx.push(a, a + 1, b, b, a + 1, b + 1);
      }
    }
    return { pos, idx };
  }

  function removeFallback() {
    if (fallbackBtn && fallbackBtn.parentNode) {
      fallbackBtn.parentNode.removeChild(fallbackBtn);
    }
    fallbackBtn = null;
  }

  // A sessao XR exige ativacao do usuario (dom.vr.require-gesture), e o
  // clique aconteceu no processo do chrome -- a pagina pode nao ter ativacao
  // transiente. Nesse caso oferecemos um alvo de clique DENTRO da pagina,
  // degradando para o caminho que ja sabemos funcionar.
  function showFallback(mode) {
    if (fallbackBtn) {
      return;
    }
    fallbackBtn = document.createElement("button");
    fallbackBtn.textContent = "Toque para ver em VR";
    fallbackBtn.style.cssText =
      "position:fixed;left:50%;top:50%;transform:translate(-50%,-50%);" +
      "z-index:2147483647;padding:22px 34px;font:bold 24px system-ui;" +
      "background:#2e7d32;color:#fff;border:none;border-radius:12px;" +
      "cursor:pointer";
    fallbackBtn.addEventListener("click", function () { start(mode); });
    document.body.appendChild(fallbackBtn);
    report("aguardando toque na pagina");
  }


  function cancelCallbacks(run) {
    if (run.videoCallback !== null) {
      run.video.cancelVideoFrameCallback(run.videoCallback);
      run.videoCallback = null;
    }
    if (run.animationFrame !== null && run.session) {
      run.session.cancelAnimationFrame(run.animationFrame);
      run.animationFrame = null;
    }
  }

  function dispose(run) {
    if (run.disposed) {
      return;
    }
    run.disposed = true;
    run.cancelled = true;
    cancelCallbacks(run);
    for (const remove of run.listeners) {
      remove();
    }
    if (run.gl) {
      for (const [method, object] of run.resources) {
        run.gl[method](object);
      }
    }
    run.resources.length = 0;
    if (active === run) {
      active = null;
    }
    report("sessao liberada, " + run.frames + " quadros");
  }

  function listen(run, target, type, listener) {
    target.addEventListener(type, listener);
    run.listeners.push(() => target.removeEventListener(type, listener));
  }

  function shutdown(run) {
    run.cancelled = true;
    cancelCallbacks(run);
    if (!run.session) {
      // requestSession may still resolve; start owns its eventual cleanup.
      return Promise.resolve();
    }
    if (!run.ending) {
      run.ending = run.session.end().catch(error => {
        report("falha ao encerrar sessao: " + error);
      }).finally(() => dispose(run));
    }
    return run.ending;
  }

  async function start(mode = "360") {
    if (active) {
      return;
    }
    const vid = document.querySelector("video");
    if (!vid) {
      report("nenhum <video> na pagina");
      return;
    }
    const meshMode = mode === "180-mesh";
    let projection = meshMode ? meshMetadata.get(vid) : null;
    if (meshMode && !projection) {
      report("VR180: malha ainda indisponivel. Recarregue o video ou troque a qualidade e tente novamente.");
      return;
    }
    if (projection && projection.pose.some(x => x !== 0)) {
      report("VR180: orientacao prhd nao neutra ainda nao suportada.");
      return;
    }
    const run = {
      video: vid, gl: null, session: null, cancelled: false, disposed: false,
      animationFrame: null, videoCallback: null, ending: null, frames: 0,
      resources: [], listeners: [],
    };
    active = run;
    let stage = "renderer";
    try {
      if (vid.paused) {
        vid.play().catch(error => report("reproducao recusada: " + error));
      }
      const canvas = document.createElement("canvas");
      const gl = canvas.getContext("webgl", { xrCompatible: true, alpha: false });
      if (!gl) {
        throw new Error("sem contexto WebGL");
      }
      run.gl = gl;
      const own = (method, object) => {
        if (!object) {
          throw new Error("falha ao alocar recurso WebGL");
        }
        run.resources.push([method, object]);
        return object;
      };
      listen(run, canvas, "webglcontextlost", () => {
        report("contexto WebGL perdido");
        shutdown(run);
      });
      const compile = (source, type) => {
        const shader = own("deleteShader", gl.createShader(type));
        gl.shaderSource(shader, source);
        gl.compileShader(shader);
        if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
          throw new Error(gl.getShaderInfoLog(shader));
        }
        return shader;
      };
      const program = own("deleteProgram", gl.createProgram());
      const meshVS = [
        "attribute vec3 aPos; attribute vec2 aUV; varying vec2 vUV;",
        "uniform mat4 uProj, uView; uniform float uYaw; uniform vec4 uCrop;",
        "void main(){vec3 p=normalize(aPos)*10.0;float c=cos(uYaw),s=sin(uYaw);",
        "p=vec3(c*p.x-s*p.z,p.y,s*p.x+c*p.z);",
        "vec2 uv=aUV*uCrop.xy+uCrop.zw;vUV=vec2(uv.x,1.0-uv.y);",
        "gl_Position=uProj*uView*vec4(p,1.0);}",
      ].join("\n");
      const meshFS = "precision highp float;varying vec2 vUV;uniform sampler2D uTex;" +
        "void main(){gl_FragColor=texture2D(uTex,vUV);}";
      gl.attachShader(program, compile(meshMode ? meshVS : VS, gl.VERTEX_SHADER));
      gl.attachShader(program, compile(meshMode ? meshFS : FS, gl.FRAGMENT_SHADER));
      gl.linkProgram(program);
      if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
        throw new Error(gl.getProgramInfoLog(program));
      }
      const geo = sphere();
      const posBuf = own("deleteBuffer", gl.createBuffer());
      gl.bindBuffer(gl.ARRAY_BUFFER, posBuf);
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(geo.pos), gl.STATIC_DRAW);
      const idxBuf = own("deleteBuffer", gl.createBuffer());
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, idxBuf);
      gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint16Array(geo.idx), gl.STATIC_DRAW);
      const tex = own("deleteTexture", gl.createTexture());
      gl.bindTexture(gl.TEXTURE_2D, tex);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA,
                    gl.UNSIGNED_BYTE, new Uint8Array([0, 0, 0, 255]));

      stage = "requestSession";
      run.session = await navigator.xr.requestSession("immersive-vr");
      const session = run.session;
      listen(run, session, "end", () => dispose(run));
      if (run.cancelled) {
        await shutdown(run);
        return;
      }
      stage = "setup";
      removeFallback();
      await gl.makeXRCompatible();
      if (run.cancelled) {
        return;
      }
      const layer = new XRWebGLLayer(session, gl, { depth: false, stencil: false });
      session.updateRenderState({ baseLayer: layer });
      const refSpace = await session.requestReferenceSpace("local");
      if (run.cancelled) {
        return;
      }
      report("sessao iniciada, video " + vid.videoWidth + "x" + vid.videoHeight);
      let frames = 0, t0 = 0, tPrev = 0, maxGap = 0, lateFrames = 0, uploads = 0;
      let pendingFrame = true, lastVideoTime = -1;
      const perf = { intervals: [], uploadMs: [], callbackMs: [], decodeMs: [],
        callbacks: 0, coalesced: 0, waiting: 0, texture: [1, 1], views: [] };
      let lastPresented = null, missedCallbacks = 0;
      const summarize = values => {
        if (!values.length) return null;
        values.sort((a, b) => a - b);
        return { n: values.length, p50: values[Math.floor((values.length - 1) * .5)],
          p95: values[Math.floor((values.length - 1) * .95)],
          p99: values[Math.floor((values.length - 1) * .99)], max: values[values.length - 1] };
      };
      const hasRvfc = typeof vid.requestVideoFrameCallback === "function" &&
                      typeof vid.cancelVideoFrameCallback === "function";
      if (hasRvfc) {
        const onVideoFrame = (_now, metadata) => {
          run.videoCallback = null;
          if (run.cancelled) {
            return;
          }
          if (pendingFrame) perf.coalesced++;
          perf.callbacks++;
          if (Number.isFinite(metadata?.processingDuration) && perf.decodeMs.length < 600) {
            perf.decodeMs.push(metadata.processingDuration * 1000);
          }
          if (Number.isFinite(metadata?.presentedFrames)) {
            if (lastPresented !== null) missedCallbacks += Math.max(0, metadata.presentedFrames - lastPresented - 1);
            lastPresented = metadata.presentedFrames;
          }
          pendingFrame = true;
          run.videoCallback = vid.requestVideoFrameCallback(onVideoFrame);
        };
        run.videoCallback = vid.requestVideoFrameCallback(onVideoFrame);
      }
      const aPos = gl.getAttribLocation(program, "aPos");
      const aUV = meshMode ? gl.getAttribLocation(program, "aUV") : -1;
      const uCrop = meshMode ? gl.getUniformLocation(program, "uCrop") : null;
      let meshBuffers = [];
      function loadMesh(next) {
        if (next.pose.some(x => x !== 0)) throw new Error("orientacao prhd nao neutra");
        for (const eye of meshBuffers) for (const sub of eye) {
          gl.deleteBuffer(sub.buffer);
          const at = run.resources.findIndex(r => r[1] === sub.buffer);
          if (at >= 0) run.resources.splice(at, 1);
        }
        meshBuffers = [];
        for (const eye of next.meshes) {
          const parts = []; meshBuffers.push(parts);
          for (const sub of eye) {
            const buffer = own("deleteBuffer", gl.createBuffer());
            gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
            gl.bufferData(gl.ARRAY_BUFFER, sub.data, gl.STATIC_DRAW);
            parts.push({buffer, count: sub.count,
              mode: [gl.TRIANGLES, gl.TRIANGLE_STRIP, gl.TRIANGLE_FAN][sub.mode]});
          }
        }
        projection = next;
        report("VR180 usando malha: " + next.width + "x" + next.height +
          ", olhos=" + next.meshes.length + ", stereo=" + next.stereo);
      }
      if (meshMode) loadMesh(projection);
      const uProj = gl.getUniformLocation(program, "uProj");
      const uView = gl.getUniformLocation(program, "uView");
      const uTex = gl.getUniformLocation(program, "uTex");
      const uYaw = gl.getUniformLocation(program, "uYaw");
      const uKind = gl.getUniformLocation(program, "uKind");
      let yawOffset = 0;
      // Etapa 2A: a projecao e escolhida pelo sinal que o YouTube declara.
      // Medido com youtube_projection.py: o rollercoaster (tR8ZtyhSDYw) declara
      // MESH e o "Jet Skydive" (kmCag9aeWWQ) declara EQUIRECTANGULAR, ambos em
      // 7680x3840. A proporcao e identica nos dois e nao serve para distinguir.
      let kind = 0;
      let kindW = -1;
      let kindH = -1;
      // Etapa 2B-a: ja houve escolha feita por sinal nesta sessao?
      let kindChosen = false;

      // Tres cuidados vindos da pesquisa (PESQUISA_PROJECAO_YOUTUBE.md secao 3):
      //  - so formatos de video contam: entradas de audio dizem RECTANGULAR;
      //  - a resposta tem de ser do video da URL, porque o objeto inicial nao
      //    acompanha a navegacao interna do YouTube;
      //  - exige consenso entre os formatos, em vez de crer no primeiro.
      function readProjectionSignal(w, h) {
        try {
          const player = document.getElementById("movie_player");
          let resp = null;
          if (player && typeof player.getPlayerResponse === "function") {
            resp = player.getPlayerResponse();
          }
          if (!resp) {
            resp = window.ytInitialPlayerResponse || null;
          }
          if (typeof resp === "string") {
            try { resp = JSON.parse(resp); } catch (e) { resp = null; }
          }
          if (!resp) {
            return { type: null, why: "sem resposta do player" };
          }

          const urlId = new URLSearchParams(location.search).get("v");
          const respId = resp.videoDetails && resp.videoDetails.videoId;
          if (urlId && respId && urlId !== respId) {
            return { type: null, why: "resposta e do video " + respId };
          }

          const sd = resp.streamingData || {};
          const all = [].concat(sd.adaptiveFormats || [], sd.formats || []);
          const video = [];
          for (const f of all) {
            if (f && f.mimeType && f.mimeType.indexOf("video/") === 0 &&
                f.projectionType) {
              video.push(f);
            }
          }
          if (!video.length) {
            return { type: null, why: "nenhum formato de video declara projecao" };
          }

          // Etapa 2B. Medido na 2A: dentro da pagina os formatos de video
          // discordam (EQUIRECTANGULAR e RECTANGULAR), embora a consulta HTTP
          // externa veja todos EQUIRECTANGULAR. A pesquisa (secao 7.1) manda
          // usar o formato ativo e a correspondencia com o frame: vale a
          // projecao dos formatos com a resolucao que o <video> decodifica.
          // A resolucao nao detecta a projecao -- identifica o que esta tocando.
          const distinct = function (list) {
            const out = [];
            for (const f of list) {
              if (out.indexOf(f.projectionType) < 0) {
                out.push(f.projectionType);
              }
            }
            return out;
          };
          // Em discordancia, o relato mostra quem declara o que, para que uma
          // nova falha traga a causa na mesma rodada.
          const breakdown = function (list) {
            const groups = {};
            for (const f of list) {
              const k = f.projectionType;
              if (!groups[k]) {
                groups[k] = [];
              }
              groups[k].push(f.itag + ":" + f.width + "x" + f.height);
            }
            return Object.keys(groups).map(function (k) {
              const items = groups[k];
              return k + " " + items.length + " [" + items.slice(0, 6).join(" ") +
                     (items.length > 6 ? " ..." : "") + "]";
            }).join("; ");
          };

          if (w > 0 && h > 0) {
            const active = video.filter(function (f) {
              return f.width === w && f.height === h;
            });
            if (active.length) {
              const kinds = distinct(active);
              if (kinds.length === 1) {
                return {
                  type: kinds[0],
                  why: active.length + " formato(s) na resolucao ativa " + w +
                       "x" + h + " concordam",
                };
              }
              return {
                type: null,
                why: "formatos na resolucao ativa " + w + "x" + h +
                     " discordam: " + breakdown(active),
              };
            }
          }

          const kinds = distinct(video);
          if (kinds.length === 1) {
            return {
              type: kinds[0],
              why: "nenhum formato em " + w + "x" + h + "; " + video.length +
                   " formatos concordam",
            };
          }
          return {
            type: null,
            why: "nenhum formato em " + w + "x" + h + " e formatos discordam: " +
                 breakdown(video),
          };
        } catch (e) {
          return { type: null, why: "falha ao ler: " + e };
        }
      }

      // Sem sinal confiavel, MANTEM o EAC -- o comportamento validado. O backup
      // antigo recorria a proporcao 2:1 para escolher ERP, mas o rollercoaster
      // em 8K tambem e 2:1 e e MESH: esse fallback quebraria o que funciona.
      function chooseKind(w, h) {
        kindW = w;
        kindH = h;
        const sig = readProjectionSignal(w, h);
        let why;
        if (sig.type === "EQUIRECTANGULAR" ||
            sig.type === "EQUIRECTANGULAR_THREED_TOP_BOTTOM") {
          kind = 1;
          why = "projectionType=" + sig.type + " (" + sig.why + ")";
        } else if (sig.type === "MESH") {
          // MESH e familia, nao layout. O perfil EAC do ADR-30 e o unico
          // validado em hardware para essa familia.
          kind = 0;
          why = "projectionType=MESH (" + sig.why + "); perfil EAC do ADR-30";
        } else if (sig.type === "RECTANGULAR") {
          kind = 0;
          why = "projectionType=RECTANGULAR (" + sig.why +
                "): video plano, mantendo EAC";
        } else if (kindChosen) {
          // Etapa 2B-a. Medido na 2B: na troca de qualidade o <video> passa por
          // 0x0 e nao ha resolucao ativa; voltar ao EAC nesse instante trocava
          // o amostrador de um video equirretangular por um quadro. Sem sinal,
          // a escolha anterior desta sessao continua valendo. Para video MESH
          // a escolha anterior ja e EAC: nada muda para o caminho validado.
          why = "SEM SINAL (" + sig.why + "); mantendo a escolha anterior";
        } else {
          kind = 0;
          why = "SEM SINAL (" + sig.why + "); mantendo EAC, o padrao validado";
        }
        if (sig.type) {
          kindChosen = true;
        }
        report("amostrador " + (kind ? "ERP" : "EAC") + " para " + w + "x" + h +
               " -- " + why);
      }

      if (!meshMode) chooseKind(vid.videoWidth, vid.videoHeight);
      let headYaw = 0;
      let initialCenterPending = true;
      let centerReason = "initial";
      listen(run, refSpace, "reset", () => { initialCenterPending = true; });

      const draw = function (time, frame) {
        run.animationFrame = null;
        if (run.cancelled) {
          return;
        }
        try {
          run.animationFrame = session.requestAnimationFrame(draw);
          const callbackStart = performance.now();
          const pose = frame.getViewerPose(refSpace);
          if (!pose) {
            return;
          }
          frames++;
          run.frames = frames;
          const q = pose.transform.orientation;
          const heading = pose.transform.matrix;
          headYaw = Math.atan2(heading[8], heading[10]);

          const validHeading = Number.isFinite(headYaw) &&
            Math.hypot(q.x, q.y, q.z, q.w) > 0.9 &&
            Math.hypot(heading[8], heading[10]) > 0.01;

          // A media sobre 300 quadros esconde rajadas -- e rajada e exatamente o
          // que o usuario descreve. O que interessa e o PIOR intervalo e quantos
          // quadros estouraram o prazo, nao o fps medio.
          if (tPrev > 0) {
            const dt = time - tPrev;
            perf.intervals.push(dt);
            if (dt > maxGap) {
              maxGap = dt;
            }
            if (dt > 16.0) {
              lateFrames++;
            }
          }
          tPrev = time;

          if (meshMode && vid.videoWidth && vid.videoHeight) {
            const next = meshMetadata.get(vid);
            if (!next) {
              report("VR180: aguardando malha da nova qualidade; volte ao painel e tente novamente.");
              shutdown(run);
              return;
            }
            if (next !== projection) loadMesh(next);
          }
          gl.bindTexture(gl.TEXTURE_2D, tex);
          const novo = hasRvfc ? pendingFrame : vid.currentTime !== lastVideoTime;
          if (vid.readyState >= 2 && vid.videoWidth && vid.videoHeight && novo) {
            const uploadStart = performance.now();
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, vid);
            perf.uploadMs.push(performance.now() - uploadStart);
            perf.texture = [vid.videoWidth, vid.videoHeight];
            pendingFrame = false;
            lastVideoTime = vid.currentTime;
            uploads++;
          }
          if (initialCenterPending && uploads > 0 && validHeading) {
            yawOffset = -headYaw;
            initialCenterPending = false;
            report("visao centralizada (" + centerReason + "), quadro " + frames +
                   ", yaw " + (headYaw * 180 / Math.PI).toFixed(1));
          }
          if (vid.readyState < 2) perf.waiting++;
          perf.views = [];
          gl.bindFramebuffer(gl.FRAMEBUFFER, layer.framebuffer);
          gl.clearColor(0, 0, 0, 1);
          gl.clear(gl.COLOR_BUFFER_BIT);
          gl.useProgram(program);
          gl.bindBuffer(gl.ARRAY_BUFFER, posBuf);
          gl.enableVertexAttribArray(aPos);
          gl.vertexAttribPointer(aPos, 3, gl.FLOAT, false, 0, 0);
          gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, idxBuf);
          gl.uniform1i(uTex, 0);
          gl.uniform1f(uYaw, yawOffset);
          // Etapa 2A: o YouTube troca de representacao e a resolucao muda; so
          // nesse momento o sinal e relido -- a leitura nao e barata.
          if (!meshMode && (vid.videoWidth !== kindW || vid.videoHeight !== kindH)) {
            chooseKind(vid.videoWidth, vid.videoHeight);
          }
          gl.uniform1f(uKind, kind);

          for (const view of pose.views) {
            const vp = layer.getViewport(view);
            if (frames === 1 || frames % 300 === 0) {
              perf.views.push([view.eye, vp.width, vp.height]);
            }
            gl.viewport(vp.x, vp.y, vp.width, vp.height);
            gl.uniformMatrix4fv(uProj, false, view.projectionMatrix);
            // 3DoF: so a rotacao da cabeca escolhe a direcao. A translacao e
            // zerada porque a esfera acompanha o espectador.
            const m = Array.from(view.transform.inverse.matrix);
            m[12] = 0;
            m[13] = 0;
            m[14] = 0;
            gl.uniformMatrix4fv(uView, false, new Float32Array(m));
            if (meshMode) {
              const right = view.eye === "right";
              let crop = [1, 1, 0, 0];
              if (projection.stereo === 1) crop = [1, 0.5, 0, right ? 0 : 0.5];
              else if (projection.stereo === 2) crop = [0.5, 1, right ? 0.5 : 0, 0];
              else if (projection.stereo === 4) crop = [0.5, 1, right ? 0 : 0.5, 0];
              gl.uniform4fv(uCrop, crop);
              const eye = meshBuffers[right && meshBuffers.length === 2 ? 1 : 0];
              gl.enableVertexAttribArray(aUV);
              for (const sub of eye) {
                gl.bindBuffer(gl.ARRAY_BUFFER, sub.buffer);
                gl.vertexAttribPointer(aPos, 3, gl.FLOAT, false, 20, 0);
                gl.vertexAttribPointer(aUV, 2, gl.FLOAT, false, 20, 12);
                gl.drawArrays(sub.mode, 0, sub.count);
              }
            } else {
              gl.drawElements(gl.TRIANGLES, geo.idx.length, gl.UNSIGNED_SHORT, 0);
            }
          }
          perf.callbackMs.push(performance.now() - callbackStart);
          if (frames === 1 || frames % 300 === 0) {
            let quality = null;
            try { quality = vid.getVideoPlaybackQuality(); } catch (_) {}
            report("perf " + JSON.stringify({ schema: 1, frame: frames,
              source: [vid.videoWidth, vid.videoHeight], texture: perf.texture,
              format: "RGBA8", depthStencilRequested: false, antialias: layer.antialias, windowMs: perf.intervals.reduce((s, v) => s + v, 0),
              framebuffer: [layer.framebufferWidth, layer.framebufferHeight],
              views: perf.views, sampler: meshMode ? "MESH" : kind ? "ERP" : "EAC",
              intervalMs: summarize(perf.intervals), uploadWallMs: summarize(perf.uploadMs),
              callbackWallMs: summarize(perf.callbackMs), processingMs: summarize(perf.decodeMs),
              videoCallbacks: perf.callbacks, coalescedCallbacks: perf.coalesced,
              missedCallbacks, waitingFrames: perf.waiting,
              videoTotal: quality?.totalVideoFrames, videoDropped: quality?.droppedVideoFrames }));
            perf.intervals = []; perf.uploadMs = []; perf.callbackMs = []; perf.decodeMs = [];
            perf.callbacks = 0; perf.coalesced = 0; perf.waiting = 0; missedCallbacks = 0;
          const error = gl.getError();
          if (error !== gl.NO_ERROR) {
            throw new Error("erro WebGL " + error);
          }
        }
        // A razao envios/quadros e a medida da mudanca: esperado cerca de 1/3
          // com video de 30 fps. Se vier perto de 1, o gatilho de quadro novo nao
          // esta funcionando e a mudanca nao teve efeito.
          if (frames === 1) {
            t0 = time;
          } else if (frames % 300 === 0) {
            const fps = (frames - 1) * 1000 / Math.max(1, time - t0);
            let decoder = "";
            try {
              const q2 = vid.getVideoPlaybackQuality
                ? vid.getVideoPlaybackQuality()
                : null;
              if (q2) {
                decoder = ", decodificador " + q2.droppedVideoFrames + "/" +
                          q2.totalVideoFrames + " perdidos";
              }
            } catch (e) {
              decoder = ", decodificador ilegivel";
            }
            let entradas = "?";
            try {
              entradas = String(session.inputSources.length);
            } catch (e) {
              entradas = "ilegivel";
            }
            report("em VR, " + frames + " quadros, " + fps.toFixed(1) + " fps, " +
                   uploads + " envios" + (hasRvfc ? " (rVFC)" : " (currentTime)") +
                   ", video " + vid.videoWidth + "x" + vid.videoHeight + decoder +
                   ", pior intervalo " + maxGap.toFixed(1) + " ms, " +
                   lateFrames + " atrasados, " + entradas + " controles");
            maxGap = 0;
            lateFrames = 0;
          }
        } catch (error) {
          report("falha no quadro: " + error);
          shutdown(run);
        }
      };

      listen(run, session, "select", () => {
        initialCenterPending = true;
        centerReason = "trigger";
        report("centralizacao solicitada; aguardando pose do proximo quadro");
      });
      listen(run, session, "squeeze", () => {
        report("saida pelo grip do controle");
        shutdown(run);
      });
      listen(run, session, "inputsourceschange", () => {
        report("controles atualizados: " + session.inputSources.length);
      });
      run.animationFrame = session.requestAnimationFrame(draw);
    } catch (error) {
      const cancelled = run.cancelled;
      report("falha em " + stage + ": " + error);
      if (run.session) {
        await shutdown(run);
      } else {
        dispose(run);
      }
      if (!cancelled && stage === "requestSession" && error.name === "SecurityError") {
        showFallback(mode);
      }
    }
  }

  function stop() {
    removeFallback();
    return active ? shutdown(active) : Promise.resolve();
  }

  return { start, stop, destroy() { meshMetadata.stop(); return stop(); } };
})();
`;

export class FxRMediaChild extends JSWindowActorChild {
  #control = null;
  #statusWindow = null;
  #statusListener = null;
  #startTimer = null;
  #starting = false;

  receiveMessage(message) {
    switch (message.name) {
      case "FxRMedia:Project":
        this.#project(message.data?.mode);
        break;
      case "FxRMedia:Exit":
        this.#exit();
        break;
    }
    return undefined;
  }

  handleEvent(event) {
    if (event.type === "DOMDocElementInserted" &&
        /(^|\.)youtube\.com$/.test(this.contentWindow.location.hostname)) {
      try { this.#ensureControl(); } catch (e) { this.#report("captura MSE indisponivel: " + e); }
    }
  }

  didDestroy() {
    this.#exit();
    try { this.#control?.destroy().catch(() => {}); } catch (e) {}
    if (this.#statusWindow && this.#statusListener) {
      try {
        this.#statusWindow.removeEventListener(
          "fxr-media-status", this.#statusListener, true
        );
      } catch (e) {}
    }
    this.#control = null;
    this.#statusWindow = null;
    this.#statusListener = null;
  }

  // O relato sobe para o chrome: e a unica forma de ver o que aconteceu sem
  // tirar o headset para abrir o devtools.
  async #reportDecoder() {
    try {
      const video = this.contentWindow?.document.querySelector("video");
      if (typeof video?.mozRequestDebugInfo !== "function") {
        this.#report("decoder: diagnostico nativo indisponivel");
        return;
      }
      const info = await video.mozRequestDebugInfo();
      const reader = info.decoder?.reader;
      if (!reader) {
        this.#report("decoder: informacao ainda indisponivel");
        return;
      }
      this.#report("decoder " + JSON.stringify({ codec: reader.videoType,
        name: reader.videoDecoderName, hardware: reader.videoHardwareAccelerated,
        width: reader.videoWidth, height: reader.videoHeight, fps: reader.videoRate }));
    } catch (error) {
      this.#report("decoder: consulta falhou: " + error);
    }
  }

  #report(text) {
    try {
      this.sendAsyncMessage("FxRMedia:Status", { text });
    } catch (e) {
      // A janela pode ja ter sido destruida; nao ha a quem relatar.
    }
  }

  #ensureControl() {
    if (this.#control) {
      return this.#control;
    }
    const win = this.contentWindow;
    if (!win) {
      return null;
    }
    // O primeiro argumento e a JANELA, nao [principal]. Passar a janela da ao
    // sandbox o principal DELA -- mesma origem, sem Xray entre sandbox e
    // pagina. Um array cria principal EXPANDIDO, que enxerga a pagina por
    // Xray, e todo quadro morre ao ler view.transform.inverse.matrix.
    const sandbox = Cu.Sandbox(win, {
      sandboxName: "FxR media projection renderer",
      sandboxPrototype: win,
      sameZoneAs: win,
      wantXrays: false,
    });
    Cu.evalInSandbox(MESH_SOURCE, sandbox, null, "FxRMeshMetadata", 1);
    Cu.evalInSandbox(RENDERER_SOURCE, sandbox, null, "FxRMediaRenderer", 1);

    // O relato do renderer chega por evento na janela de conteudo.
    this.#statusWindow = win;
    this.#statusListener = event => this.#report(event.detail);
    win.addEventListener("fxr-media-status", this.#statusListener, true);

    // Sem waiveXrays o objeto chega, mas sem os metodos: este ator e codigo
    // de sistema e enxerga um objeto de principal de pagina atraves de Xray,
    // que nao expoe as funcoes.
    this.#control = Cu.waiveXrays(sandbox.__fxrControl);
    return this.#control;
  }

  #project(mode = "360") {
    if (this.#starting) {
      this.#report("entrada em VR em andamento; aguardando resposta do runtime");
      return;
    }
    let control;
    try {
      control = this.#ensureControl();
    } catch (e) {
      this.#report("falha ao injetar o renderer: " + e);
      return;
    }
    if (!control) {
      this.#report("sem janela de conteudo");
      return;
    }
    try {
      // Etapa 1C-e. requestSession imersivo exige ativacao transiente na
      // pagina (XRSystem.cpp:144-148; janela de 5 s em
      // dom.user_activation.transient.timeout). O clique do usuario acontece
      // na barra do FxR, no chrome, e nao conta como gesto na pagina -- por
      // isso a entrada direta era intermitente (medido: entrava direto so
      // quando a pagina tinha recebido um clique nos 5 s anteriores). Este
      // ator e codigo de sistema e concede a ativacao so para esta acao do
      // usuario, sem desligar dom.vr.require-gesture para os demais sites.
      try {
        this.document.notifyUserGestureActivation();
        this.#report("ativacao de gesto concedida pela acao na barra");
      } catch (e) {
        this.#report("notifyUserGestureActivation falhou: " + e);
      }
      this.#starting = true;
      const win = this.contentWindow;
      this.#startTimer = win.setTimeout(() => {
        this.#report("entrada em VR excedeu 15 s; cancelando pedido pendente");
        try { win.navigator.xr.cancelPendingSession(); }
        catch (e) { this.#report("cancelamento pendente falhou: " + e); }
      }, 15000);
      this.#reportDecoder();
      control.start(mode).catch(e => this.#report("start falhou: " + e)).finally(() => {
        this.#starting = false;
        if (this.#startTimer !== null) {
          win.clearTimeout(this.#startTimer);
          this.#startTimer = null;
        }
      });
    } catch (e) {
      this.#report("start falhou: " + e);
    }
  }

  #exit() {
    if (this.#startTimer !== null) {
      try { this.#statusWindow?.clearTimeout(this.#startTimer); } catch (e) {}
      this.#startTimer = null;
    }
    if (this.#starting) {
      try {
        this.contentWindow.navigator.xr.cancelPendingSession();
      } catch (e) {
        this.#report("cancelamento pendente falhou: " + e);
      }
    }
    if (this.#control) {
      try {
        this.#control.stop().catch(e => this.#report("stop falhou: " + e));
      } catch (e) {
        // A sessao pode ja ter terminado sozinha; nada a desfazer.
      }
    }
  }
}
