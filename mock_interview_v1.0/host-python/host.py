import json
import os
import base64
import io
import struct
import wave
import asyncio
import uuid
import hashlib
import hmac
import datetime
from urllib.parse import urlparse
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.request import Request, urlopen
from urllib.error import HTTPError

ROOT = Path(__file__).resolve().parent

def load_env():
    p = ROOT / '.env'
    if p.exists():
        for line in p.read_text(encoding='utf-8').splitlines():
            if '=' in line and not line.lstrip().startswith('#'):
                k, v = line.split('=', 1)
                os.environ.setdefault(k.strip(), v.strip().strip('"'))

load_env()
API_KEY = os.getenv('DEEPSEEK_API_KEY', '')
ENDPOINT = os.getenv('DEEPSEEK_ENDPOINT', 'https://api.deepseek.com/chat/completions')
MODEL = os.getenv('DEEPSEEK_MODEL', 'deepseek-chat')
ASR_URL = os.getenv('ASR_URL', '')
ASR_KEY = os.getenv('ASR_KEY', '')
TTS_URL = os.getenv('TTS_URL', '')
TTS_KEY = os.getenv('TTS_KEY', '')
ASR_MODE = os.getenv('ASR_MODE', 'raw').lower()
ASR_FIELD = os.getenv('ASR_FIELD', 'file')
TTS_FORMAT = os.getenv('TTS_FORMAT', 'pcm16').lower()
DASHSCOPE_KEY = os.getenv('DASHSCOPE_API_KEY', '')
ASR_PROVIDER = os.getenv('ASR_PROVIDER', '').lower()
TTS_PROVIDER = os.getenv('TTS_PROVIDER', '').lower()
ASR_MODEL = os.getenv('ASR_MODEL', 'paraformer-realtime-v2')
TTS_MODEL = os.getenv('TTS_MODEL', 'sambert-zhichu-v1')
TENCENT_SECRET_ID = os.getenv('TENCENT_SECRET_ID', '')
TENCENT_SECRET_KEY = os.getenv('TENCENT_SECRET_KEY', '')
TENCENT_REGION = os.getenv('TENCENT_REGION', 'ap-beijing')
VISION_URL = os.getenv('VISION_URL', '')
VISION_KEY = os.getenv('VISION_KEY', '')
VISION_MODEL = os.getenv('VISION_MODEL', '')
VISION_PROVIDER = os.getenv('VISION_PROVIDER', '').lower()

SYSTEM = ('你是一名客观、严格、专业的中文模拟面试官。'
          '根据面试主题一次只问一个问题。收到回答后，评价相关性、准确性、完整性、表达清晰度、语气词和时长。'
          '必须给出0到100分、优点、问题和2到3条具体改进建议。不要替候选人回答问题。')

def ask_deepseek(messages):
    if not API_KEY:
        raise RuntimeError('DEEPSEEK_API_KEY is not configured in host-python/.env')
    body = json.dumps({'model': MODEL, 'messages': messages, 'temperature': 0.3}).encode()
    req = Request(ENDPOINT, data=body, headers={'Content-Type': 'application/json', 'Authorization': f'Bearer {API_KEY}'})
    with urlopen(req, timeout=45) as resp:
        data = json.loads(resp.read().decode('utf-8'))
    return data['choices'][0]['message']['content']

def analyze_vision(image_base64, context=''):
    provider = VISION_PROVIDER
    if provider == 'dashscope' or (not provider and DASHSCOPE_KEY and not VISION_URL):
        vision_url = VISION_URL or 'https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions'
        vision_key = VISION_KEY or DASHSCOPE_KEY
        vision_model = VISION_MODEL or 'qwen-vl-plus'
    else:
        vision_url, vision_key, vision_model = VISION_URL, VISION_KEY, VISION_MODEL
    if not vision_url or not vision_key or not vision_model:
        raise RuntimeError('VISION_URL, VISION_KEY and VISION_MODEL are required')
    prompt = ('分析这张面试中的人像画面，只返回简洁中文 JSON：'
              '{"expression":"自然/紧张/僵硬/无法判断","movement":"正常/偏多/明显过多/无法判断",'
              '"tension":"低/中/高/无法判断","advice":"一句具体建议"}。'
              '不要推断身份、年龄、性别或健康状况。' + (f'上下文：{context}' if context else ''))
    body = json.dumps({'model': vision_model, 'messages': [{'role': 'user', 'content': [{'type': 'text', 'text': prompt}, {'type': 'image_url', 'image_url': {'url': 'data:image/jpeg;base64,' + image_base64}}]}], 'temperature': 0.1}).encode()
    req = Request(vision_url, data=body, headers={'Content-Type': 'application/json', 'Authorization': f'Bearer {vision_key}'})
    with urlopen(req, timeout=60) as resp: data = json.loads(resp.read().decode('utf-8'))
    return data['choices'][0]['message']['content']

def analyze_vision_trend(images, context=''):
    if not images:
        raise RuntimeError('no vision frames supplied')
    provider = VISION_PROVIDER
    if provider == 'dashscope' or (not provider and DASHSCOPE_KEY and not VISION_URL):
        vision_url = VISION_URL or 'https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions'
        vision_key = VISION_KEY or DASHSCOPE_KEY
        vision_model = VISION_MODEL or 'qwen-vl-plus'
    else:
        vision_url, vision_key, vision_model = VISION_URL, VISION_KEY, VISION_MODEL
    if not vision_url or not vision_key or not vision_model:
        raise RuntimeError('VISION_URL, VISION_KEY and VISION_MODEL are required')
    prompt = ('这是同一场模拟面试中按时间顺序采集的多张人像画面。请只返回简洁中文 JSON：'
              '{"expression_trend":"自然/逐渐紧张/明显紧张/无法判断",'
              '"movement_trend":"稳定/逐渐增多/明显过多/无法判断",'
              '"tension_trend":"低/中/高/无法判断","presence_score":0,'
              '"summary":"一句总结","advice":"一句最重要的改进建议"}。'
              '必须基于多帧变化判断趋势，不要推断身份、年龄、性别或健康状况。'
              + (f'面试上下文：{context}' if context else ''))
    content = [{'type': 'text', 'text': prompt}]
    for image in images[:12]:
        content.append({'type': 'image_url', 'image_url': {'url': 'data:image/jpeg;base64,' + image}})
    body = json.dumps({'model': vision_model, 'messages': [{'role': 'user', 'content': content}], 'temperature': 0.1}).encode()
    req = Request(vision_url, data=body, headers={'Content-Type': 'application/json', 'Authorization': f'Bearer {vision_key}'})
    with urlopen(req, timeout=90) as resp: data = json.loads(resp.read().decode('utf-8'))
    return data['choices'][0]['message']['content']

def transcribe(pcm):
    if ASR_PROVIDER == 'tencent':
        return tencent_asr(pcm)
    if ASR_PROVIDER == 'dashscope' or (DASHSCOPE_KEY and ASR_MODEL.startswith('paraformer')):
        return dashscope_transcribe(pcm)
    if not ASR_URL:
        raise RuntimeError('ASR_URL is not configured in host-python/.env')
    headers = {'Content-Type': 'audio/pcm;rate=16000;channels=1'}
    if ASR_KEY: headers['Authorization'] = f'Bearer {ASR_KEY}'
    data = pcm
    if ASR_MODE == 'json':
        headers['Content-Type'] = 'application/json'
        data = json.dumps({'audio_base64': base64.b64encode(pcm).decode(), 'format': 'pcm16', 'sample_rate': 16000, 'channels': 1}).encode()
    elif ASR_MODE == 'multipart':
        boundary = '----rorolee-asr-boundary'
        headers['Content-Type'] = f'multipart/form-data; boundary={boundary}'
        head = (f'--{boundary}\r\nContent-Disposition: form-data; name="{ASR_FIELD}"; filename="audio.pcm"\r\nContent-Type: audio/pcm\r\n\r\n').encode()
        data = head + pcm + f'\r\n--{boundary}--\r\n'.encode()
    req = Request(ASR_URL, data=data, headers=headers)
    try:
        with urlopen(req, timeout=60) as resp: data = json.loads(resp.read().decode('utf-8'))
    except HTTPError as exc:
        detail = exc.read().decode('utf-8', errors='replace')
        raise RuntimeError(f'DashScope TTS HTTP {exc.code}: {detail}') from exc
    return data.get('text') or data.get('transcript') or data.get('result') or ''

def synthesize(text):
    if TTS_PROVIDER == 'tencent':
        return tencent_tts(text)
    if TTS_PROVIDER == 'dashscope' or (DASHSCOPE_KEY and TTS_MODEL):
        return dashscope_synthesize(text)
    if not TTS_URL:
        raise RuntimeError('TTS_URL is not configured in host-python/.env')
    body = json.dumps({'text': text, 'format': 'pcm16', 'sample_rate': 16000, 'channels': 1}).encode()
    headers = {'Content-Type': 'application/json'}
    if TTS_KEY: headers['Authorization'] = f'Bearer {TTS_KEY}'
    req = Request(TTS_URL, data=body, headers=headers)
    with urlopen(req, timeout=60) as resp:
        raw = resp.read(); ctype = resp.headers.get('Content-Type','')
    if 'application/json' in ctype or raw[:1] in (b'{', b'['):
        data = json.loads(raw.decode('utf-8'))
        b64 = data.get('audio_base64') or data.get('audio') or data.get('data')
        raw = base64.b64decode(b64) if b64 else b''
    return pcm16_from_audio(raw)

async def _dashscope_asr(pcm):
    import websockets
    task_id = str(uuid.uuid4())
    uri = os.getenv('DASHSCOPE_ASR_WS', 'wss://dashscope.aliyuncs.com/api-ws/v1/inference')
    headers = {'Authorization': f'Bearer {DASHSCOPE_KEY}'}
    async with websockets.connect(uri, additional_headers=headers, max_size=None) as ws:
        await ws.send(json.dumps({'header': {'action': 'run-task', 'task_id': task_id, 'streaming': 'duplex'}, 'payload': {'task_group': 'audio', 'task': 'asr', 'function': 'recognition', 'model': ASR_MODEL, 'parameters': {'format': 'pcm', 'sample_rate': 16000}, 'input': {}}}))
        first = json.loads(await ws.recv())
        if first.get('header', {}).get('event') not in ('task-started', 'task-started-event') and first.get('header', {}).get('status') not in ('20000000', None):
            raise RuntimeError(f'DashScope ASR start failed: {first}')
        for off in range(0, len(pcm), 3200):
            await ws.send(pcm[off:off + 3200])
        await ws.send(json.dumps({'header': {'action': 'finish-task', 'task_id': task_id, 'streaming': 'duplex'}, 'payload': {'input': {}}}))
        text = ''
        async for message in ws:
            if isinstance(message, bytes):
                continue
            data = json.loads(message)
            output = data.get('payload', {}).get('output', {})
            sentence = output.get('sentence', output) if isinstance(output, dict) else {}
            text = sentence.get('text') or output.get('text') or text
            if data.get('header', {}).get('event') in ('task-finished', 'task-finished-event'):
                break
        return text

def dashscope_transcribe(pcm):
    if not DASHSCOPE_KEY: raise RuntimeError('DASHSCOPE_API_KEY is not configured')
    return asyncio.run(_dashscope_asr(pcm))

def dashscope_synthesize(text):
    if not DASHSCOPE_KEY: raise RuntimeError('DASHSCOPE_API_KEY is not configured')
    endpoint = os.getenv('DASHSCOPE_TTS_URL', 'https://dashscope.aliyuncs.com/api/v1/services/aigc/text-to-audio/generation')
    body = json.dumps({'model': TTS_MODEL, 'input': {'text': text}, 'parameters': {'format': 'wav', 'sample_rate': 16000}}).encode()
    req = Request(endpoint, data=body, headers={'Content-Type': 'application/json', 'Authorization': f'Bearer {DASHSCOPE_KEY}'})
    try:
        with urlopen(req, timeout=60) as resp: data = json.loads(resp.read().decode('utf-8'))
    except HTTPError as exc:
        detail = exc.read().decode('utf-8', errors='replace')
        raise RuntimeError(f'DashScope TTS HTTP {exc.code}: {detail}') from exc
    audio = data.get('output', {}).get('audio', {})
    audio_url = audio.get('url') if isinstance(audio, dict) else None
    if not audio_url: raise RuntimeError(f'DashScope TTS returned no audio URL: {data}')
    with urlopen(Request(audio_url), timeout=60) as resp: raw = resp.read()
    return pcm16_from_audio(raw)

def tencent_request(service, action, version, payload):
    if not TENCENT_SECRET_ID or not TENCENT_SECRET_KEY:
        raise RuntimeError('TENCENT_SECRET_ID/TENCENT_SECRET_KEY are not configured')
    host = f'{service}.tencentcloudapi.com'; endpoint = f'https://{host}'
    timestamp = int(datetime.datetime.now(datetime.timezone.utc).timestamp()); date = datetime.datetime.fromtimestamp(timestamp, datetime.timezone.utc).strftime('%Y-%m-%d')
    body = json.dumps(payload, separators=(',', ':'), ensure_ascii=False)
    ct = 'application/json; charset=utf-8'; canonical_headers = f'content-type:{ct}\nhost:{host}\n'; signed_headers = 'content-type;host'
    canonical_request = f'POST\n/\n\n{canonical_headers}\n{signed_headers}\n{hashlib.sha256(body.encode()).hexdigest()}'
    credential_scope = f'{date}/{service}/tc3_request'; hashed = hashlib.sha256(canonical_request.encode()).hexdigest(); string_to_sign = f'TC3-HMAC-SHA256\n{timestamp}\n{credential_scope}\n{hashed}'
    def sign(key, msg): return hmac.new(key, msg.encode(), hashlib.sha256).digest()
    secret_date = sign(('TC3' + TENCENT_SECRET_KEY).encode(), date); secret_service = sign(secret_date, service); secret_signing = sign(secret_service, 'tc3_request'); signature = hmac.new(secret_signing, string_to_sign.encode(), hashlib.sha256).hexdigest()
    auth = f'TC3-HMAC-SHA256 Credential={TENCENT_SECRET_ID}/{credential_scope}, SignedHeaders={signed_headers}, Signature={signature}'
    req = Request(endpoint, data=body.encode(), headers={'Content-Type': ct, 'Host': host, 'X-TC-Action': action, 'X-TC-Version': version, 'X-TC-Region': TENCENT_REGION, 'X-TC-Timestamp': str(timestamp), 'Authorization': auth})
    try:
        with urlopen(req, timeout=60) as resp: return json.loads(resp.read().decode('utf-8'))
    except HTTPError as exc:
        raise RuntimeError(f'Tencent {action} HTTP {exc.code}: {exc.read().decode("utf-8", errors="replace")}') from exc

def tencent_asr(pcm):
    data = {'ProjectId': 0, 'SubServiceType': 2, 'EngSerViceType': os.getenv('TENCENT_ASR_ENGINE', '16k_zh'), 'SourceType': 1, 'VoiceFormat': 'pcm', 'Data': base64.b64encode(pcm).decode(), 'DataLen': len(pcm)}
    result = tencent_request('asr', 'SentenceRecognition', '2019-06-14', data)
    if result.get('Response', {}).get('Error'): raise RuntimeError(str(result['Response']['Error']))
    return result.get('Response', {}).get('Result', '')

def tencent_tts(text):
    data = {'Text': text, 'SessionId': uuid.uuid4().hex, 'VoiceType': int(os.getenv('TENCENT_TTS_VOICE', '101001')), 'Codec': 'pcm', 'SampleRate': 16000, 'PrimaryLanguage': 1}
    result = tencent_request('tts', 'TextToVoice', '2019-08-23', data)
    response = result.get('Response', {})
    if response.get('Error'): raise RuntimeError(str(response['Error']))
    raw = base64.b64decode(response.get('Audio', ''))
    return pcm16_from_audio(raw)

def pcm16_from_audio(raw):
    """Normalize cloud TTS output to signed PCM16, 16 kHz, mono."""
    if not raw:
        raise RuntimeError('TTS returned an empty audio payload')
    if raw[:4] != b'RIFF' or raw[8:12] != b'WAVE':
        if TTS_FORMAT in ('pcm', 'pcm16', 'raw'):
            return raw[:-1] if len(raw) % 2 else raw
        raise RuntimeError('TTS output is not WAV and TTS_FORMAT is not pcm16')
    with wave.open(io.BytesIO(raw), 'rb') as wav:
        channels = wav.getnchannels(); width = wav.getsampwidth(); rate = wav.getframerate(); frames = wav.readframes(wav.getnframes())
    if width != 2: raise RuntimeError(f'TTS WAV must be 16-bit PCM, got {width * 8}-bit')
    if channels == 2:
        samples = struct.unpack('<' + 'h' * (len(frames) // 2), frames)
        frames = struct.pack('<' + 'h' * (len(samples) // 2), *[sum(samples[i:i+2]) // 2 for i in range(0, len(samples), 2)])
    elif channels != 1: raise RuntimeError(f'TTS WAV must be mono or stereo, got {channels} channels')
    if rate != 16000: raise RuntimeError(f'TTS WAV must be 16000 Hz, got {rate} Hz; configure the provider accordingly')
    return frames

class Handler(BaseHTTPRequestHandler):
    def _json(self, status, value):
        raw = json.dumps(value, ensure_ascii=False).encode('utf-8')
        self.send_response(status); self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Access-Control-Allow-Origin', '*'); self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS'); self.send_header('Access-Control-Allow-Headers', 'Content-Type, Authorization'); self.send_header('Content-Length', str(len(raw))); self.end_headers(); self.wfile.write(raw)
    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header('Access-Control-Allow-Origin', '*'); self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS'); self.send_header('Access-Control-Allow-Headers', 'Content-Type, Authorization'); self.send_header('Access-Control-Max-Age', '600'); self.end_headers()
    def do_GET(self): self._json(200, {'ok': True, 'deepseek_configured': bool(API_KEY), 'asr_configured': bool(ASR_URL or (DASHSCOPE_KEY and ASR_PROVIDER == 'dashscope') or (TENCENT_SECRET_ID and ASR_PROVIDER == 'tencent')), 'tts_configured': bool(TTS_URL or (DASHSCOPE_KEY and TTS_PROVIDER == 'dashscope') or (TENCENT_SECRET_ID and TTS_PROVIDER == 'tencent')), 'vision_configured': bool((VISION_URL and VISION_KEY and VISION_MODEL) or (DASHSCOPE_KEY and (VISION_PROVIDER == 'dashscope' or not VISION_PROVIDER))), 'vision_provider': VISION_PROVIDER or ('dashscope' if DASHSCOPE_KEY else 'generic'), 'vision_model': VISION_MODEL or ('qwen-vl-plus' if DASHSCOPE_KEY else ''), 'asr_provider': ASR_PROVIDER or 'generic', 'tts_provider': TTS_PROVIDER or 'generic', 'asr_model': ASR_MODEL, 'tts_model': TTS_MODEL, 'model': MODEL})
    def do_POST(self):
        if self.path == '/vision':
            try:
                n = int(self.headers.get('Content-Length', '0')); payload = json.loads(self.rfile.read(n) or b'{}'); result = analyze_vision(payload.get('image_base64', ''), payload.get('context', '')); return self._json(200, {'ok': True, 'result': result})
            except Exception as exc: return self._json(500, {'ok': False, 'error': str(exc)})
        if self.path == '/vision/trend':
            try:
                n = int(self.headers.get('Content-Length', '0')); payload = json.loads(self.rfile.read(n) or b'{}')
                result = analyze_vision_trend(payload.get('images', []), payload.get('context', ''))
                return self._json(200, {'ok': True, 'result': result})
            except Exception as exc: return self._json(500, {'ok': False, 'error': str(exc)})
        if self.path == '/asr':
            try:
                n = int(self.headers.get('Content-Length', '0')); text = transcribe(self.rfile.read(n)); return self._json(200, {'ok': True, 'text': text})
            except Exception as exc: return self._json(500, {'ok': False, 'error': str(exc)})
        if self.path == '/tts':
            try:
                n = int(self.headers.get('Content-Length', '0')); payload = json.loads(self.rfile.read(n) or b'{}'); raw = synthesize(payload.get('text',''))
                import base64
                return self._json(200, {'ok': True, 'audio_base64': base64.b64encode(raw).decode(), 'format': 'pcm16', 'sample_rate': 16000})
            except Exception as exc: return self._json(500, {'ok': False, 'error': str(exc)})
        if self.path != '/interview': return self._json(404, {'error': 'not found'})
        try:
            n = int(self.headers.get('Content-Length', '0')); payload = json.loads(self.rfile.read(n) or b'{}')
            action = payload.get('action', 'evaluate')
            if action == 'question':
                topic = payload.get('topic', '').strip()
                prompt = (f"面试主题：{topic}\n这是第{payload.get('question_index', 1)}题，共{payload.get('total_questions', 1)}题。"
                          '请生成一个与主题相关、难度适中的面试问题，只返回问题文本。不要加序号、解释或答案。')
            elif action == 'summary':
                topic = payload.get('topic', '').strip()
                answers = payload.get('answers', [])
                prompt = (f"面试主题：{topic}\n整场面试问答记录：{json.dumps(answers, ensure_ascii=False)}\n"
                          '请对整场回答做一次最终大总结，不要逐题点评。严格返回JSON，字段为：'
                          'overall_score（0到100整数）、summary（一句话结论）、strengths（2到3条）、'
                          'weaknesses（2到3条）、examples（结合候选人原回答的具体短句举例，至少1条）、'
                          'suggestions（2到3条可执行建议）。评分综合相关性、准确性、完整性、表达清晰度、'
                          '语气词和回答时长；引用回答时保持原意，不要编造候选人未说过的内容。')
            else:
                prompt = (f"面试主题：{payload.get('topic','')}\n当前问题：{payload.get('question','')}\n"
                          f"候选人回答：{payload.get('transcript','')}\n回答时长：{payload.get('duration_sec',0)}秒\n"
                          '请严格返回JSON，字段为 score、strengths、weaknesses、suggestions、next_question。')
            result = ask_deepseek([{'role':'system','content':SYSTEM},{'role':'user','content':prompt}])
            self._json(200, {'ok': True, 'result': result})
        except Exception as exc: self._json(500, {'ok': False, 'error': str(exc)})

if __name__ == '__main__':
    print('Interview Agent listening on http://127.0.0.1:8765')
    HTTPServer(('127.0.0.1', 8765), Handler).serve_forever()
