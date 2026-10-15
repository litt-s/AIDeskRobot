const api = require('../../utils/api');

Page({
  data: {
    id: '',
    messages: [],
    generation: null,
    draft: '',
    canSend: false,
    busy: false,
    pausing: false,
    resuming: false,
    loading: true,
    scrollTop: 0
  },
  streamTask: null,
  pollTimer: null,
  assistantId: 'stream',
  onLoad(options) {
    this.setData({ id: options.id });
    this.open(options.id);
  },
  onUnload() {
    if (this.pollTimer) clearInterval(this.pollTimer);
    if (this.streamTask && this.streamTask.abort) this.streamTask.abort();
  },
  onDraftInput(e) {
    /* WXML 的 {{}} 不能调用方法：draft.trim() 会求值成 undefined，按钮就永远 disabled。这里算成布尔值给模板用。 */
    this.setData({ draft: e.detail.value, canSend: !!e.detail.value.trim() });
  },
  async open(id) {
    if (!id) return;
    try {
      const [generation, detail] = await Promise.all([
        api.request('/conversations/' + id + '/generation'),
        api.request('/conversations/' + id)
      ]);
      this.setData({
        generation: generation.generation,
        messages: detail.messages,
        loading: false,
        scrollTop: Date.now()
      });
      this.syncPolling();
    } catch (error) {
      this.setData({ loading: false });
      wx.showToast({ title: error.message, icon: 'none' });
    }
  },
  syncPolling() {
    if (this.pollTimer) clearInterval(this.pollTimer);
    if (this.data.generation && this.data.generation.state === 'running') {
      this.pollTimer = setInterval(() => this.open(this.data.id), 1500);
    }
  },
  upsertAssistant(contentDelta, nextId) {
    const messages = this.data.messages.map(item => ({ ...item }));
    let found = messages.find(item => item.id === nextId || item.id === 'stream');
    if (!found) {
      found = { id: 'stream', role: 'assistant', content: '', status: 'generating', created_at: '' };
      messages.push(found);
    }
    if (contentDelta) found.content += contentDelta;
    found.status = 'generating';
    if (nextId) found.id = nextId;
    this.assistantId = found.id;
    this.setData({ messages, scrollTop: Date.now() });
  },
  consume(path, data = {}) {
    if (this.streamTask && this.streamTask.abort) this.streamTask.abort();
    this.upsertAssistant('');
    this.setData({ busy: true });
    this.streamTask = api.stream(path, data, {
      event: event => {
        if (event.generation) {
          this.assistantId = event.generation.assistant_id;
          this.setData({ generation: event.generation });
          this.upsertAssistant('', event.generation.assistant_id);
        }
        if (event.error) {
          wx.showToast({ title: event.error, icon: 'none' });
        }
        if (event.delta) {
          this.upsertAssistant(event.delta, this.assistantId);
        }
      },
      done: async () => {
        this.setData({ busy: false, resuming: false });
        await this.open(this.data.id);
      },
      error: async error => {
        this.setData({ busy: false, resuming: false });
        wx.showToast({ title: error.message, icon: 'none' });
        await this.open(this.data.id);
      }
    });
  },
  async send() {
    const content = this.data.draft.trim();
    if (!content || this.data.busy) return;
    this.setData({ busy: true, draft: '', canSend: false });
    this.upsertAssistant('');
    try {
      this.consume('/conversations/' + this.data.id + '/chat', { content });
    } catch (error) {
      wx.showToast({ title: error.message, icon: 'none' });
      this.setData({ busy: false });
    }
  },
  async pause() {
    if (this.data.pausing) return;
    this.setData({ pausing: true });
    try {
      const result = await api.request('/conversations/' + this.data.id + '/chat/pause', 'POST', {});
      this.setData({ generation: result.generation });
      if (this.streamTask && this.streamTask.abort) this.streamTask.abort();
      await this.open(this.data.id);
    } catch (error) {
      wx.showToast({ title: error.message, icon: 'none' });
    } finally {
      this.setData({ pausing: false });
    }
  },
  async resume() {
    if (this.data.busy || this.data.resuming) return;
    this.setData({ resuming: true });
    this.consume('/conversations/' + this.data.id + '/chat/resume');
  },
  async rewind() {
    if (this.data.busy || (this.data.generation && this.data.generation.state === 'running')) return;
    wx.showModal({
      title: '撤销上一轮',
      content: '这条提问和回复会被删除，问题内容放回输入框。',
      success: async res => {
        if (!res.confirm) return;
        const result = await api.request('/conversations/' + this.data.id + '/rewind', 'POST', {});
        this.setData({ messages: result.messages, draft: result.removed || '', generation: null });
      }
    });
  },
  async regenerate() {
    if (this.data.busy || (this.data.generation && this.data.generation.state === 'running')) return;
    wx.showModal({
      title: '重新生成',
      content: '会替换这条提问当前的回复，确定继续？',
      success: res => {
        if (res.confirm) this.consume('/conversations/' + this.data.id + '/regenerate');
      }
    });
  }
});
