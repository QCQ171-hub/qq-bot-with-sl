// QQAI聊天工具 - 前端
// 直接由后端（qqai_backend）托管：http://127.0.0.1:8788/index.html
// 用 file:// 打开也能用（跨域已由后端放开），此时默认连 8788 端口。

const API_BASE = location.protocol === 'file:' || location.port === ''
    ? 'http://127.0.0.1:8788'
    : location.origin;

const state = {
    config: null,
    groups: [],
    privates: [],
    stickers: [],
    messages: [],
    pending: [],
    target: '',
    status: null,
    askBatch: null,
    seenBatches: new Set(),
    lastMsgId: null,
    userLists: { blacklist: [], whitelist: [] },
};

// ───────── 基础工具 ─────────

function $(id) { return document.getElementById(id); }
function esc(s) {
    return String(s == null ? '' : s)
        .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}
function show(id) { $(id).classList.remove('hidden'); }
function hide(id) { $(id).classList.add('hidden'); }

function toast(msg, type) {
    const box = $('toastBox');
    if (!box) return;
    const el = document.createElement('div');
    el.className = 'toast ' + (type || '');
    el.textContent = msg;
    box.appendChild(el);
    setTimeout(() => el.remove(), type === 'error' ? 7000 : 3200);
}

async function api(path, opts) {
    const o = Object.assign({ headers: { 'Content-Type': 'application/json' } }, opts || {});
    let res;
    try {
        res = await fetch(API_BASE + path, o);
    } catch (e) {
        throw new Error('连不上后端（' + API_BASE + '）。请先运行 qqai_backend.exe');
    }
    let data = {};
    try { data = await res.json(); } catch (e) { data = { ok: false, error: '后端返回不是 JSON（HTTP ' + res.status + '）' }; }
    if (!res.ok || data.ok === false) throw new Error(data.error || ('HTTP ' + res.status));
    return data;
}

function post(path, body) {
    return api(path, { method: 'POST', body: JSON.stringify(body || {}) });
}

function targetOf(id, kind) { return kind + ':' + id; }

function convName(target) {
    const [kind, id] = String(target).split(':');
    if (kind === 'group') {
        const g = state.groups.find(x => x.id === id);
        if (g) return g.name;
    } else {
        const p = state.privates.find(x => x.id === id);
        if (p) return p.name;
    }
    return target;
}

// ───────── 初始化 ─────────

document.addEventListener('DOMContentLoaded', async () => {
    try {
        await loadAll();
        startPolling();
    } catch (e) {
        toast(e.message, 'error');
    }
});

async function loadAll() {
    const [st, cfg, gp, pv, sk, ul] = await Promise.all([
        api('/api/status'),
        api('/api/config'),
        api('/api/groups'),
        api('/api/private'),
        api('/api/stickers'),
        api('/api/userlists'),
    ]);
    state.status = st;
    state.config = cfg.config;
    state.groups = gp.groups || [];
    state.privates = pv.private || [];
    state.stickers = sk.stickers || [];
    state.userLists = { blacklist: ul.blacklist || [], whitelist: ul.whitelist || [] };
    fillConfigForm(state.config);
    fillUserLists();
    renderConvs();
    renderStickers();
    renderStatus();
    if (state.groups.length && !state.target) selectTarget(targetOf(state.groups[0].id, 'group'));
    else if (state.privates.length && !state.target) selectTarget(targetOf(state.privates[0].id, 'private'));
    else { await refreshMessages(); await refreshPending(); }
    if (!state.config.model) toast('还需要输入模型名字（右侧「配置」里的模型名字）', 'error');
}

let pollTimer = null;
function startPolling() {
    if (pollTimer) clearInterval(pollTimer);
    pollTimer = setInterval(async () => {
        try {
            await Promise.all([refreshMessages(true), refreshPending(), refreshStatus()]);
        } catch (e) { /* 后端瞬时不可用，忽略 */ }
    }, 1500);
}

async function refreshStatus() {
    state.status = await api('/api/status');
    renderStatus();
    const cnt = (state.status.pending || 0);
    $('queueCount').textContent = state.status.pending + ' 条待确认 / ' + state.status.queued + ' 条排队中'
        + (state.status.simulating ? '（本地模拟）' : '（真实 QQ）');
    return cnt;
}

function renderStatus() {
    const st = state.status || {};
    $('connBackend').textContent = '在线 v' + (st.version || '?');
    $('connBackend').className = 'on';
    const ob = $('connOnebot');
    if (st.sendMode === 'simulate') { ob.textContent = '已关闭'; ob.className = ''; }
    else if (st.onebotUp) { ob.textContent = '在线 ' + (st.onebotNick || ''); ob.className = 'on'; }
    else { ob.textContent = '不在线'; ob.className = 'bad'; }
    $('connMode').textContent = st.simulating ? '本地模拟' : '真实 QQ';
    $('connMode').className = st.simulating ? 'off' : 'on';
    $('modelBadge').textContent = st.model ? ('模型 ' + st.model) : '模型未设置';
    const toggle = $('realSendToggle');
    if (toggle) toggle.checked = st.sendMode !== 'simulate';
    const at = $('autoReplyToggle');
    if (at) at.checked = st.autoReply !== false;
    const ca = $('connAuto');
    if (ca) {
        if (!st.autoReply) { ca.textContent = '关'; ca.className = ''; }
        else if (st.autoConfirm) { ca.textContent = '开（直接发）'; ca.className = 'on'; }
        else { ca.textContent = '开（需确认）'; ca.className = 'on'; }
        ca.title = st.lastAutoInfo || '';
    }
    const ct = $('connToken');
    if (ct) {
        const n = st.tokensToday || 0;
        ct.textContent = n.toLocaleString() + ' tokens';
        ct.title = '今日（' + (st.tokenDate || '') + '）调用模型累计：输入 '
            + (st.promptTokensToday || 0).toLocaleString() + ' + 输出 '
            + (st.completionTokensToday || 0).toLocaleString()
            + ' = ' + n.toLocaleString() + ' token。AI 在回复里写 %token 会替换成这个数。';
    }
    $('queueCount').textContent = (st.pending || 0) + ' 条待确认 / ' + (st.queued || 0) + ' 条排队中'
        + (st.simulating ? '（本地模拟）' : '（真实 QQ）');
}

// ───────── 会话列表 ─────────

function renderConvs() {
    const gl = $('groupList');
    gl.innerHTML = state.groups.length ? '' : '<div class="conv-sub" style="padding:6px 10px">还没有群，点上面「+ 添加」</div>';
    state.groups.forEach(g => {
        const el = document.createElement('div');
        el.className = 'conv' + (state.target === 'group:' + g.id ? ' active' : '');
        el.innerHTML = `
            <div class="conv-face">${esc((g.name || g.id).slice(0, 2))}</div>
            <div class="conv-main">
                <div class="conv-name">${esc(g.name)}</div>
                <div class="conv-sub">群号 ${esc(g.id)} · ${g.members || 0} 人</div>
            </div>
            <div class="conv-dot ${g.enabled ? 'on' : ''}" title="点击开关响应"></div>`;
        el.onclick = () => selectTarget('group:' + g.id);
        el.oncontextmenu = (ev) => { ev.preventDefault(); removeConv('group', g.id); };
        el.querySelector('.conv-dot').onclick = (ev) => { ev.stopPropagation(); toggleConv('groups', g.id, !g.enabled); };
        gl.appendChild(el);
    });

    const pl = $('privateList');
    pl.innerHTML = state.privates.length ? '' : '<div class="conv-sub" style="padding:6px 10px">还没有私聊对象</div>';
    state.privates.forEach(p => {
        const el = document.createElement('div');
        el.className = 'conv' + (state.target === 'private:' + p.id ? ' active' : '');
        el.innerHTML = `
            <div class="conv-face">${esc((p.name || p.id).slice(0, 2))}</div>
            <div class="conv-main">
                <div class="conv-name">${esc(p.name)}</div>
                <div class="conv-sub">QQ ${esc(p.id)}</div>
            </div>
            <div class="conv-dot ${p.enabled ? 'on' : ''}"></div>`;
        el.onclick = () => selectTarget('private:' + p.id);
        el.oncontextmenu = (ev) => { ev.preventDefault(); removeConv('private', p.id); };
        el.querySelector('.conv-dot').onclick = (ev) => { ev.stopPropagation(); toggleConv('private', p.id, !p.enabled); };
        pl.appendChild(el);
    });
}

function selectTarget(t) {
    state.target = t;
    renderConvs();
    $('chatTitle').textContent = convName(t);
    $('chatSub').textContent = t + '　·　Ctrl+Enter 问 AI';
    refreshMessages();
    refreshPending();
}

async function addGroup() {
    const id = prompt('群号（纯数字）');
    if (!id) return;
    const name = prompt('群名称（留空用群号）', '') || id;
    try {
        await post('/api/groups', { id: id.trim(), name: name.trim(), members: 0, enabled: true });
        toast('群聊已添加', 'success');
        state.groups = (await api('/api/groups')).groups || [];
        renderConvs();
        selectTarget('group:' + id.trim());
    } catch (e) { toast(e.message, 'error'); }
}

async function addPrivate() {
    const id = prompt('对方 QQ 号（纯数字）');
    if (!id) return;
    const name = prompt('备注名（留空用 QQ 号）', '') || id;
    try {
        await post('/api/private', { id: id.trim(), name: name.trim(), enabled: true });
        toast('私聊已添加', 'success');
        state.privates = (await api('/api/private')).private || [];
        renderConvs();
        selectTarget('private:' + id.trim());
    } catch (e) { toast(e.message, 'error'); }
}

async function syncGroups() {
    try {
        const r = await post('/api/groups/sync', {});
        state.groups = (await api('/api/groups')).groups || [];
        renderConvs();
        toast(`已从 QQ 同步群列表：新增 ${r.added} 个，共 ${r.total} 个`, 'success');
    } catch (e) { toast(e.message, 'error'); }
}

async function syncPrivates() {
    try {
        const r = await post('/api/private/sync', {});
        state.privates = (await api('/api/private')).private || [];
        renderConvs();
        toast(`已从 QQ 同步好友列表：新增 ${r.added} 个，共 ${r.total} 个`, 'success');
    } catch (e) { toast(e.message, 'error'); }
}

async function removeConv(kind, id) {
    if (!confirm('从列表移除这个会话？（只删本地记录，不会退群）')) return;
    try {
        await post(kind === 'group' ? '/api/groups/delete' : '/api/private/delete', { id });
        if (kind === 'group') state.groups = (await api('/api/groups')).groups || [];
        else state.privates = (await api('/api/private')).private || [];
        if (state.target === kind + ':' + id) { state.target = ''; $('chatTitle').textContent = '选择一个会话'; }
        renderConvs();
    } catch (e) { toast(e.message, 'error'); }
}

async function toggleConv(kind, id, enabled) {    try {
        await post(kind === 'groups' ? '/api/groups' : '/api/private', { id, enabled });
        if (kind === 'groups') state.groups = (await api('/api/groups')).groups || [];
        else state.privates = (await api('/api/private')).private || [];
        renderConvs();
    } catch (e) { toast(e.message, 'error'); }
}

// ───────── 消息流 ─────────

async function refreshMessages(silent) {
    if (!state.target) { $('messagesContainer').innerHTML = '<div class="empty-tip">左侧选择一个群聊或私聊。</div>'; return; }
    try {
        const data = await api('/api/messages?target=' + encodeURIComponent(state.target) + '&limit=300');
        state.messages = data.messages || [];
        renderMessages();
    } catch (e) { if (!silent) toast(e.message, 'error'); }
}

function fmtClock(ms) {
    if (!ms) return '';
    const d = new Date(ms);
    const now = new Date();
    const p = n => String(n).padStart(2, '0');
    const hm = p(d.getHours()) + ':' + p(d.getMinutes());
    return d.toDateString() === now.toDateString() ? hm : (p(d.getMonth() + 1) + '-' + p(d.getDate()) + ' ' + hm);
}

function countdown(ms) {
    const left = ms - Date.now();
    if (left <= 0) return '发送中…';
    const s = Math.round(left / 1000);
    if (s < 60) return s + ' 秒后';
    if (s < 3600) return Math.round(s / 60) + ' 分钟后';
    return Math.round(s / 3600) + ' 小时后';
}

function stickerById(id) { return state.stickers.find(s => s.id === id); }

function itemHtml(m) {
    if (m.type === 'image') {
        const st = stickerById(m.stickerId);
        const url = st ? st.url : m.content;
        const label = st ? (st.note || st.name || ('表情#' + st.id)) : ('表情#' + m.stickerId);
        if (url && /^(https?:|data:|\/|\.{1,2}\/)/.test(url)) {
            return `<img src="${esc(url)}" alt="${esc(label)}" onerror="this.outerHTML='<span class=&quot;sticker-fallback&quot;>[图片无法显示 ${esc(label)}]</span>'">`;
        }
        return `<span class="sticker-fallback">[${esc(label)}]</span>`;
    }
    return esc(m.content);
}

function statusChip(m) {
    const map = {
        pending: '待确认（还发不发）',
        queued: '排队中',
        sent: '已发送',
        failed: '发送失败',
        cancelled: '已取消',
    };
    let chip = `<span class="chip ${esc(m.status)}">${esc(map[m.status] || m.status)}</span>`;
    if (m.status === 'queued' && m.epoch) chip += `<span class="chip timer">${esc(countdown(m.epoch))}</span>`;
    if (m.dir === 'out' && m.status === 'sent' && state.status && state.status.simulating)
        chip += '<span class="chip sim">模拟</span>';
    return chip;
}

function renderMessages() {
    const box = $('messagesContainer');
    const nearBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 120;
    if (!state.messages.length) {
        box.innerHTML = '<div class="empty-tip">这个会话还没有消息。<br>在下方输入一句话点「问 AI」，AI 会用 <code>{send:…}</code> 格式决定发什么。</div>';
        return;
    }
    const persona = (state.config && state.config.persona) || 'AI';
    box.innerHTML = state.messages.map((m, i) => {
        const mine = m.dir === 'out';
        const who = mine ? (m.sender || persona) : (m.sender || '群友');
        const time = fmtClock(m.status === 'queued' ? m.epoch : (m.createdAt || m.epoch));
        const sched = m.timeRaw ? `<span class="chip timer">定时 ${esc(m.timeRaw)}</span>` : '';
        const err = m.error ? `<div class="meta" style="color:#a5271d">${esc(m.error)}</div>` : '';
        return `<div class="msg ${mine ? 'out' : 'in'}">
            <div class="avatar" title="右键：加好友 / 黑白名单" oncontextmenu="avatarMenu(event, ${i})">${esc(who.slice(0, 2))}</div>
            <div class="bubble">
                <div class="meta">${esc(who)} ${esc(time)} ${statusChip(m)} ${sched}</div>
                ${itemHtml(m)}
                ${err}
            </div>
        </div>`;
    }).join('');
    if (nearBottom || !box.dataset.keepScroll) box.scrollTop = box.scrollHeight;
}

async function clearMessages() {
    if (!state.target) return toast('先选择一个会话', 'error');
    if (!confirm('清空该会话的消息记录？')) return;
    try {
        await post('/api/messages/clear', { target: state.target });
        await refreshMessages();
        toast('已清空', 'success');
    } catch (e) { toast(e.message, 'error'); }
}

$('messagesContainer') && $('messagesContainer').addEventListener('scroll', e => {
    e.currentTarget.dataset.keepScroll = '1';
});

// ───────── 待确认（还发不发） ─────────

async function refreshPending() {
    const data = await api('/api/pending');
    state.pending = data.batches || [];
    renderQueue();
    // 新出现的批次自动弹「还发不发」
    const fresh = state.pending.find(b => !state.seenBatches.has(b.batchId));
    state.pending.forEach(b => state.seenBatches.add(b.batchId));
    if (fresh && $('askModal').classList.contains('hidden')) openAsk(fresh.batchId);
}

function renderQueue() {
    const box = $('queueList');
    const queued = state.messages.filter(m => m.status === 'queued');
    if (!state.pending.length && !queued.length) {
        box.innerHTML = '<div class="empty-tip" style="margin:14px 0">没有等你确认的消息。</div>';
        return;
    }
    let html = state.pending.map(b => `
        <div class="queue-item">
            <div class="q-head"><span>${esc(b.targetName || b.target)} · ${esc(b.createdText)}</span>
                <span>
                    <button class="mini" onclick="openAsk('${esc(b.batchId)}')">还发不发</button>
                    <button class="mini danger" onclick="cancelBatch('${esc(b.batchId)}')">不发</button>
                </span>
            </div>
            ${b.items.map(it => `<div class="q-line">
                ${it.type === 'image' ? `<img src="${esc(it.content)}" onerror="this.style.display='none'">` : ''}
                <span class="txt">${it.type === 'image' ? '表情#' + it.stickerId : (it.atId ? '@' + it.atId + ' ' : '') + esc(it.content)}</span>
                <span class="chip ${it.immediate ? 'sent' : 'queued'}">${esc(it.timeText)}</span>
            </div>`).join('')}
        </div>`).join('');
    html += queued.map(m => `
        <div class="queue-item">
            <div class="q-head"><span>排队中 · ${esc(m.targetName || m.target)}</span>
                <span><button class="mini danger" onclick="dropQueued('${esc(m.id)}')">撤回这条</button></span></div>
            <div class="q-line"><span class="txt">${m.type === 'image' ? '表情#' + m.stickerId : esc(m.content)}</span>
                <span class="chip queued">${esc(fmtClock(m.epoch))}（${esc(countdown(m.epoch))}）</span></div>
        </div>`).join('');
    box.innerHTML = html;
}

function openAsk(batchId) {
    const b = state.pending.find(x => x.batchId === batchId);
    if (!b) return toast('这批消息已经不在了', 'error');
    state.askBatch = batchId;
    $('askTarget').textContent = `目标：${b.targetName || b.target}（${b.target}）　共 ${b.items.length} 条`;
    $('askItems').innerHTML = b.items.map(it => `
        <div class="pv">
            ${it.type === 'image'
                ? `<img src="${esc(it.content)}" alt="表情${it.stickerId}" onerror="this.outerHTML='<span class=&quot;sticker-fallback&quot;>[表情 #${it.stickerId} 无法显示]</span>'">`
                : (it.atId ? '@' + it.atId + ' ' : '') + esc(it.content)}
            <span class="when">${it.immediate ? '立即发送（与其它条目同时）' : '定时 ' + esc(it.timeText) + '（原文 time:' + esc(it.timeRaw) + '）'}</span>
        </div>`).join('');
    $('askRaw').textContent = '确认后进入发送队列；点「不发」则整批丢弃。';
    show('askModal');
}

function closeAsk() { hide('askModal'); state.askBatch = null; }

async function confirmAsk(send) {
    const b = state.askBatch;
    if (!b) return closeAsk();
    try {
        const r = await post('/api/confirm', { batchId: b, action: send ? 'send' : 'cancel' });
        (r.expired || []).forEach(e => toast(e, 'error'));
        toast(send ? `已排进发送队列（${r.count} 条）` : '这批不发了', send ? 'success' : '');
    } catch (e) { toast(e.message, 'error'); }
    closeAsk();
    await Promise.all([refreshMessages(true), refreshPending()]);
}

async function cancelBatch(batchId) {
    try {
        await post('/api/confirm', { batchId, action: 'cancel' });
        toast('已取消这批', 'success');
        if (state.askBatch === batchId) closeAsk();
        await Promise.all([refreshMessages(true), refreshPending()]);
    } catch (e) { toast(e.message, 'error'); }
}

async function cancelAll() {
    try {
        const r = await post('/api/cancel-all');
        toast('已取消 ' + r.count + ' 条待确认消息', 'success');
        closeAsk();
        await Promise.all([refreshMessages(true), refreshPending()]);
    } catch (e) { toast(e.message, 'error'); }
}

async function dropQueued(id) {
    try {
        await post('/api/queue/delete', { id });
        toast('该条已不再发送', 'success');
        await Promise.all([refreshMessages(true), refreshPending()]);
    } catch (e) { toast(e.message, 'error'); }
}

// ───────── 输入区动作 ─────────

function requireTarget() {
    if (!state.target) { toast('先在左侧选择一个群聊或私聊', 'error'); return false; }
    return true;
}

async function askAI(btn) {
    if (!requireTarget()) return;
    const text = $('messageInput').value.trim();
    if (!text) return toast('要说点什么？（这就是提示词里的 %s）', 'error');
    if (btn) btn.disabled = true;
    try {
        const r = await post('/api/chat', {
            target: state.target,
            text,
            name: $('speakerName').value.trim() || '群友',
            id: $('speakerId').value.trim(),
            model: $('modelOverride').value.trim(),
        });
        await Promise.all([refreshMessages(true), refreshPending()]);
        if (r.silent) {
            toast('AI 这次没写 {send:…}，按规则不发消息。\n原文：' + r.reply);
        } else {
            (r.errors || []).forEach(e => toast(e, 'error'));
        }
    } catch (e) {
        toast(e.message, 'error');
        if (e.message && /模型/.test(e.message)) switchTab('config', document.querySelector('.tab'));
    } finally {
        if (btn) btn.disabled = false;
    }
}

async function previewParse() {
    const text = $('messageInput').value;
    if (!text.trim()) return toast('输入框是空的', 'error');
    try {
        const r = await post('/api/parse', { text });
        $('previewItems').innerHTML = (r.items || []).length
            ? r.items.map(it => `<div class="pv">${it.type === 'image'
                ? `<img src="${esc(it.content)}">` : esc(it.content)}
                <span class="when">${it.immediate ? '立即' : '定时 ' + esc(it.timeText)}</span></div>`).join('')
            : '<div class="empty-tip">没解析出发送条目 —— 按规则这就是「不发消息」。</div>';
        $('previewErrors').textContent = (r.errors || []).join('\n') || '没有错误';
        show('previewModal');
    } catch (e) { toast(e.message, 'error'); }
}

async function manualEnqueue() {
    if (!requireTarget()) return;
    const text = $('messageInput').value.trim();
    if (!text) return toast('输入内容为空', 'error');
    try {
        const r = await post('/api/enqueue', { target: state.target, text });
        (r.errors || []).forEach(e => toast(e, 'error'));
        state.seenBatches.delete(r.batchId);  // 让轮询弹出确认框
        await Promise.all([refreshMessages(true), refreshPending()]);
    } catch (e) { toast(e.message, 'error'); }
}

async function sendDirect() {
    if (!requireTarget()) return;
    const text = $('messageInput').value.trim();
    if (!text) return toast('输入内容为空', 'error');
    try {
        await post('/api/send', { target: state.target, type: 'send', content: text });
        $('messageInput').value = '';
        await refreshMessages(true);
        toast(state.status && state.status.simulating ? '已进队列（本地模拟发送）' : '已进队列，按「队列间隔」节奏发送', 'success');
    } catch (e) { toast(e.message, 'error'); }
}

async function insertSticker(id) {
    const box = $('messageInput');
    box.value += (box.value && !/[{:]$/.test(box.value) ? '' : '') + `{image:${id}}`;
    box.focus();
    toast('已插入 {image:' + id + '}');
}

async function toggleAutoReply(on) {
    try {
        const cfg = await post('/api/config', { autoReply: on });
        state.config = cfg.config;
        fillConfigForm(cfg.config);
        await refreshStatus();
        toast(on ? '自动收信已开启：后端会盯着群/好友消息，按分寸接话' : '自动收信已关闭', 'success');
    } catch (e) { toast(e.message, 'error'); }
}

async function toggleSendMode(real) {
    try {
        await post('/api/config', { sendMode: real ? 'auto' : 'simulate' });
        const cfg = await api('/api/config');
        state.config = cfg.config;
        fillConfigForm(cfg.config);
        await refreshStatus();
        toast(real ? '已切换：OneBot 在线时真实发送到 QQ' : '已切换：只在本地模拟，不触达 QQ', 'success');
    } catch (e) { toast(e.message, 'error'); }
}

// ───────── 黑名单 / 白名单 / 右键头像 ─────────

function fillUserLists() {
    if (!$('blacklistInput')) return;
    $('blacklistInput').value = (state.userLists.blacklist || []).join(', ');
    $('whitelistInput').value = (state.userLists.whitelist || []).join(', ');
}

async function reloadUserLists() {
    const ul = await api('/api/userlists');
    state.userLists = { blacklist: ul.blacklist || [], whitelist: ul.whitelist || [] };
    fillUserLists();
}

async function saveUserLists() {
    try {
        await post('/api/userlists', {
            blacklist: $('blacklistInput').value,
            whitelist: $('whitelistInput').value,
        });
        await reloadUserLists();
        toast('黑白名单已保存（按 QQ 号记录，黑名单优先）', 'success');
    } catch (e) { toast(e.message, 'error'); }
}

async function avatarMenu(ev, idx) {
    ev.preventDefault();
    const m = state.messages[idx];
    if (!m) return;
    const who = m.sender || '此人';
    let id = m.senderId || '';
    if (!id) {
        id = prompt(who + ' 的 QQ 号（这条消息没带 id，手输一个）：');
        if (!id) return;
    }
    id = String(id).trim();
    if (!/^\d+$/.test(id)) return toast('QQ 号要纯数字：' + id, 'error');
    const ul = state.userLists;
    const inB = (ul.blacklist || []).includes(id);
    const inW = (ul.whitelist || []).includes(id);
    const isFriend = (state.privates || []).some(p => p.id === id);
    const menu = '对 ' + who + '（QQ ' + id + '）：\n\n'
        + '1 申请好友' + (isFriend ? '（已是好友，将跳过）' : '（仅对陌生人有效）') + '\n'
        + '2 ' + (inW ? '移出白名单' : '加入白名单') + (inW ? '' : '（之后只回白名单里的人）') + '\n'
        + '3 ' + (inB ? '移出黑名单' : '加入黑名单') + (inB ? '' : '（不再回复 TA）');
    const act = prompt(menu, '');
    if (act === '1') {
        if (isFriend) return toast(who + '（' + id + '）已经是好友', 'error');
        try {
            const r = await post('/api/friend/add', { id });
            toast(r.simulated ? '本地模拟模式：没有真的发好友申请' : '好友申请已提交（若 OneBot 不支持主动加好友会有明确报错）', 'success');
        } catch (e) { toast(e.message, 'error'); }
    } else if (act === '2' || act === '3') {
        try {
            const r = await post('/api/userlists/toggle', { list: act === '2' ? 'white' : 'black', id });
            await reloadUserLists();
            toast((r.inList ? '已加入 ' : '已移出 ') + (act === '2' ? '白名单' : '黑名单') + '：' + id, 'success');
        } catch (e) { toast(e.message, 'error'); }
    }
}

// ───────── 配置面板 ─────────

function switchTab(name, btn) {
    document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
    document.querySelectorAll('.tab-body').forEach(t => t.classList.add('hidden'));
    show('tab-' + name);
    const target = btn || Array.from(document.querySelectorAll('.tab'))
        .find(t => t.getAttribute('onclick').includes("'" + name + "'"));
    if (target) target.classList.add('active');
}

function fillConfigForm(c) {
    $('apiUrl').value = c.apiUrl || '';
    $('baseUrl').value = c.baseUrl || '';
    $('model').value = c.model || '';
    $('persona').value = c.persona || '';
    $('promptTemplate').value = c.promptTemplate || '';
    $('onebotUrl').value = c.onebotUrl || '';
    $('onebotToken').value = c.onebotToken || '';
    $('sendMode').value = c.sendMode || 'auto';
    $('sendIntervalMs').value = c.sendIntervalMs != null ? c.sendIntervalMs : 0;
    $('maxTokens').value = c.maxTokens || 1200;
    $('temperature').value = c.temperature != null ? c.temperature : 0.9;
    $('attachFormatHint').checked = c.attachFormatHint !== false;
    $('attachStickerList').checked = c.attachStickerList !== false;
    $('realSendToggle').checked = (c.sendMode || 'auto') !== 'simulate';
    if ($('autoReplyToggle')) $('autoReplyToggle').checked = c.autoReply !== false;
    $('autoCheckMs').value = c.autoCheckMs != null ? c.autoCheckMs : 5000;
    $('replyProbability').value = c.replyProbability != null ? c.replyProbability : 30;
    $('replyCooldownMs').value = c.replyCooldownMs != null ? c.replyCooldownMs : 60000;
    $('contextWindow').value = c.contextWindow != null ? c.contextWindow : 15;
    $('mustReplyKeywords').value = (c.mustReplyKeywords || []).join(',');
    $('autoConfirm').checked = c.autoConfirm !== false;
    $('appendHint').checked = c.appendHint !== false;
    $('privateAlwaysReply').checked = c.privateAlwaysReply !== false;
    if ($('stickerEdge')) $('stickerEdge').value = c.stickerLongEdge != null ? c.stickerLongEdge : 32;
    if ($('stickerCount')) $('stickerCount').value = c.stickerCount != null ? c.stickerCount : 60;
}

function collectConfig() {
    return {
        apiUrl: $('apiUrl').value.trim(),
        baseUrl: $('baseUrl').value.trim(),
        model: $('model').value.trim(),
        persona: $('persona').value,
        promptTemplate: $('promptTemplate').value,
        onebotUrl: $('onebotUrl').value.trim(),
        onebotToken: $('onebotToken').value.trim(),
        sendMode: $('sendMode').value,
        sendIntervalMs: Number($('sendIntervalMs').value || 0),
        maxTokens: Number($('maxTokens').value || 1200),
        temperature: Number($('temperature').value || 0.9),
        attachFormatHint: $('attachFormatHint').checked,
        attachStickerList: $('attachStickerList').checked,
        autoReply: $('autoReplyToggle') ? $('autoReplyToggle').checked : true,
        autoCheckMs: Number($('autoCheckMs').value || 5000),
        replyProbability: Number($('replyProbability').value || 30),
        replyCooldownMs: Number($('replyCooldownMs').value || 60000),
        contextWindow: Number($('contextWindow').value || 15),
        mustReplyKeywords: $('mustReplyKeywords').value.split(/[,，]/).map(s => s.trim()).filter(Boolean),
        autoConfirm: $('autoConfirm').checked,
        appendHint: $('appendHint').checked,
        privateAlwaysReply: $('privateAlwaysReply').checked,
        stickerLongEdge: Number($('stickerEdge') ? $('stickerEdge').value || 32 : 32),
        stickerCount: Number($('stickerCount') ? $('stickerCount').value || 60 : 60),
    };
}

async function saveConfig() {
    const c = collectConfig();
    if (!c.model) return toast('需要输入模型名字（例如 glm-4-flash）', 'error');
    try {
        const r = await post('/api/config', c);
        state.config = r.config;
        state.status = r.status || state.status;
        renderStatus();
        fillConfigForm(r.config);
        if (r.restarting) {
            toast('配置已保存，程序热重启中…', 'success');
            const back = await waitBackendBack();
            if (back) {
                await refreshStatus();
                toast('热重启完成，新配置已生效', 'success');
            } else {
                toast('程序已退出重启，但 20 秒内未恢复响应，请稍后刷新页面', 'error');
            }
        } else {
            toast('配置已保存到 config.json', 'success');
        }
    } catch (e) { toast(e.message, 'error'); }
}

// 等旧进程退出、新进程起来后 /api/status 恢复 200
async function waitBackendBack(maxMs = 20000) {
    const t0 = Date.now();
    await new Promise(res => setTimeout(res, 1500));
    while (Date.now() - t0 < maxMs) {
        try { await api('/api/status'); return true; } catch (e) { /* 还没起来，继续等 */ }
        await new Promise(res => setTimeout(res, 1000));
    }
    return false;
}

function restoreDefaultPrompt() {
    $('promptTemplate').value = (state.config && state.config.defaultPromptTemplate) ||
        '%name发送了%s，用格式可以发表情包{image:编号}，没写{send:内容}=不发消息，这个代表发消息，可以发很多条，同时发，如果有send/image就会询问还发不发，使用{send:内容,time:时间（YYMMDD+4位小时分钟，也可以只写4位小时分钟今天发送，如果比现在的时间早要报错，image也可以这么写）';
    toast('已恢复默认提示词');
}

async function testAI() {
    try {
        const r = await post('/api/test/ai', { model: $('model').value.trim() });
        toast(`模型 ${r.model} 正常（${r.latencyMs}ms）：${r.reply}`, 'success');
    } catch (e) { toast(e.message, 'error'); }
}

async function testQQ() {
    try {
        const r = await post('/api/test/qq', {});
        toast('OneBot 在线：' + (r.nickname || '(未取到昵称)'), 'success');
    } catch (e) {
        toast(e.message + '\nOneBot 不在线时会自动退回本地模拟发送。', 'error');
    }
    await refreshStatus();
}

// ───────── 表情包 ─────────

function renderStickers() {
    const grid = $('stickersGrid');
    $('stickerHint').textContent = `共 ${state.stickers.length} 个，编号用于 {image:编号}`;
    if (!state.stickers.length) {
        grid.innerHTML = '<div class="empty-tip" style="grid-column:1/-1;margin:10px 0">还没有表情包。<br>点「从 QQ 同步收藏表情」拉取账号收藏，或手动添加图片地址。</div>';
        return;
    }
    grid.innerHTML = state.stickers.map(s => `
        <div class="sticker" title="点击插入 {image:${s.id}}${s.emojiId ? '  emoji_id=' + s.emojiId : ''}">
            <span class="num">${s.id}</span>
            ${s.favorite ? '<span class="fav">★</span>' : ''}
            ${s.url ? `<img src="${esc(s.url)}" loading="lazy" onerror="this.outerHTML='<div class=&quot;no-img&quot;>图挂了</div>'">`
                : '<div class="no-img">无地址</div>'}
            <div class="nm">${esc(s.note || s.name || s.description || '表情')}</div>
        </div>`).join('');
    Array.from(grid.children).forEach((el, i) => {
        const s = state.stickers[i];
        el.onclick = () => insertSticker(s.id);
        el.oncontextmenu = async (ev) => {
            ev.preventDefault();
            const act = prompt(`表情 #${s.id}：1 收藏/取消  2 改备注  3 改名  4 删除`, '1');
            if (act === '1') { await post('/api/stickers/favorite', { id: s.id }); await reloadStickers(); }
            else if (act === '2') {
                const note = prompt('备注（会喂给模型，说明这张表情适合在什么场合用）', s.note || '');
                if (note !== null) { await post('/api/stickers', { id: s.id, url: s.url, note }); await reloadStickers(); }
            } else if (act === '3') {
                const name = prompt('名称', s.name || '');
                if (name !== null) { await post('/api/stickers', { id: s.id, url: s.url, name }); await reloadStickers(); }
            } else if (act === '4') {
                if (confirm('删除表情 #' + s.id + '？')) { await post('/api/stickers/delete', { id: s.id }); await reloadStickers(); }
            }
        };
    });
}

async function reloadStickers() {
    state.stickers = (await api('/api/stickers')).stickers || [];
    renderStickers();
    await refreshStatus();
}

async function syncStickers() {
    try {
        const r = await post('/api/stickers/sync', {});
        state.stickers = r.stickers || [];
        renderStickers();
        toast(`同步完成：新增 ${r.added} 个，共 ${r.total} 个`, 'success');
    } catch (e) {
        toast(e.message + '\nOneBot 不可用时可以先「手动添加」图片地址。', 'error');
    }
}

async function addSticker() {
    const url = prompt('图片地址（QQ 表情 URL / 本地 /static 下的路径 / 网络图片）');
    if (!url) return;
    const name = prompt('名称（给模型看的说明越具体越好）', '') || '';
    try {
        await post('/api/stickers', { url: url.trim(), name: name.trim() });
        await reloadStickers();
        toast('表情包已添加', 'success');
    } catch (e) { toast(e.message, 'error'); }
}

// ───────── 快捷键 ─────────

document.addEventListener('keydown', e => {
    if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') { e.preventDefault(); askAI(); }
    if (e.key === 'Escape') {
        if (!$('askModal').classList.contains('hidden')) closeAsk();
        else hide('previewModal');
    }
});

window.addEventListener('beforeunload', () => { if (pollTimer) clearInterval(pollTimer); });
