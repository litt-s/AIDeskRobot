const api = require('../../utils/api');

const EMOTIONS = ['neutral','happy','sad','angry','surprised','thinking','confused','embarrassed','crying','laughing','funny','loving','shocked','cool','relaxed','delicious','kissy','confident','silly','sleepy'];
const ACTIONS = ['center','look_left','look_right','look_up','look_down','nod','shake','roll','dance','blink','wakeup','sleep'];
const STATUS = { pending: '待执行', sent: '已下发', done: '已完成', failed: '失败' };

function emptyDevice() {
  return { id: '', name: '' };
}

Page({
  data: {
    devices: [],
    roles: [],
    roleNames: [],
    editing: emptyDevice(),
    selected: null,
    commands: [],
    createdKey: '',
    speak: '',
    wake: '',
    canSpeak: false,
    canWake: false,
    volume: 60,
    emotions: EMOTIONS,
    actions: ACTIONS,
    busy: false,
    error: ''
  },
  onShow() {
    this.load();
  },
  async load() {
    try {
      const [devices, roles] = await Promise.all([
        api.request('/devices'),
        api.request('/roles')
      ]);
      this.setData({
        devices: devices.devices,
        roles: roles.roles,
        roleNames: roles.roles.map(role => role.name)
      });
      if (this.data.selected) await this.refreshDetail(this.data.selected.id);
    } catch (error) {
      this.setData({ error: error.message });
    }
  },
  startAdd() {
    this.setData({ editing: emptyDevice(), createdKey: '' });
  },
  startRename(e) {
    const id = e.currentTarget.dataset.id;
    const device = this.data.devices.find(item => item.id === id);
    if (device) this.setData({ editing: { id, name: device.name }, createdKey: '' });
  },
  cancelEdit() {
    this.setData({ editing: emptyDevice(), createdKey: '' });
  },
  onNameInput(e) {
    this.setData({ 'editing.name': e.detail.value });
  },
  onSpeakInput(e) {
    this.setData({ speak: e.detail.value, canSpeak: !!e.detail.value.trim() });
  },
  onWakeInput(e) {
    this.setData({ wake: e.detail.value, canWake: !!e.detail.value.trim() });
  },
  onVolumeChange(e) {
    this.setData({ volume: Number(e.detail.value) });
  },
  async saveDevice() {
    if (!this.data.editing.name.trim()) return;
    this.setData({ busy: true });
    try {
      if (this.data.editing.id) {
        await api.request('/devices/' + this.data.editing.id, 'PATCH', { name: this.data.editing.name });
        wx.showToast({ title: '已保存', icon: 'success' });
        this.cancelEdit();
        this.load();
      } else {
        const result = await api.request('/devices', 'POST', { name: this.data.editing.name });
        this.setData({ createdKey: result.key });
        wx.setClipboardData({ data: result.key });
        this.cancelEdit();
        this.load();
      }
    } catch (error) {
      this.setData({ error: error.message });
    } finally {
      this.setData({ busy: false });
    }
  },
  async removeDevice(e) {
    const id = e.currentTarget.dataset.id;
    wx.showModal({
      title: '解绑设备',
      content: '解绑后设备将无法连接。',
      success: async res => {
        if (!res.confirm) return;
        await api.request('/devices/' + id, 'DELETE');
        if (this.data.selected && this.data.selected.id === id) this.setData({ selected: null, commands: [] });
        this.load();
      }
    });
  },
  async selectDevice(e) {
    const id = e.currentTarget.dataset.id;
    const device = this.data.devices.find(item => item.id === id);
    if (!device) return;
    this.setData({ selected: device, speak: '', wake: '', volume: 60, canSpeak: false, canWake: false });
    await this.refreshDetail(id);
  },
  async refreshDetail(id) {
    const detail = await api.request('/devices/' + id + '/commands');
    this.setData({
      selected: detail.device,
      commands: detail.commands.map(item => ({ ...item, statusText: STATUS[item.status] || item.status }))
    });
  },
  async sendCommand(type, payload) {
    if (!this.data.selected) return;
    this.setData({ busy: true });
    try {
      await api.request('/devices/' + this.data.selected.id + '/commands', 'POST', { type, payload });
      wx.showToast({ title: '指令已下发', icon: 'none' });
      await this.refreshDetail(this.data.selected.id);
    } catch (error) {
      wx.showToast({ title: error.message, icon: 'none' });
    } finally {
      this.setData({ busy: false });
    }
  },
  onEmotionChange(e) {
    const name = this.data.emotions[Number(e.detail.value)];
    this.sendCommand('emotion', { name });
  },
  onActionChange(e) {
    const name = this.data.actions[Number(e.detail.value)];
    this.sendCommand('action', { name });
  },
  sendVolume() {
    this.sendCommand('volume', { value: this.data.volume });
  },
  sendSpeak() {
    const text = this.data.speak.trim();
    if (!text) return;
    this.sendCommand('speak', { text });
    this.setData({ speak: '', canSpeak: false });
  },
  sendListen() {
    this.sendCommand('listen', {});
  },
  sendReboot() {
    this.sendCommand('reboot', {});
  },
  onRoleChange(e) {
    const role = this.data.roles[Number(e.detail.value)];
    if (!role || !this.data.selected) return;
    this.setData({ 'selected.role_id': role.id });
    api.request('/devices/' + this.data.selected.id, 'PATCH', { name: this.data.selected.name, role_id: role.id })
      .then(() => this.load());
  },
  async sendWake() {
    const text = this.data.wake.trim();
    if (!text || !this.data.selected) return;
    try {
      await api.request('/devices/' + this.data.selected.id + '/wake', 'POST', { text });
      wx.showToast({ title: '已下发唤醒词', icon: 'none' });
      this.setData({ wake: '', canWake: false });
      this.load();
    } catch (error) {
      wx.showToast({ title: error.message, icon: 'none' });
    }
  }
});
