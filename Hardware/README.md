# AIUI 极速交互 SDK

> 本仓库中的 DeskEmoji ESP32-S3 N16R8 移植工程位于 `esp32/`。
> 硬件、配网、AIUI 配置、连续对话、音乐、表情动作、构建烧录和排障说明请先阅读
> [`esp32/README.md`](esp32/README.md)。下文保留讯飞 RTOS SDK 原始说明作为协议参考。

## 1. 接入流程

- 先登陆[AIUI开放平台](https://aiui.xfyun.cn)，创建AIUI应用
- 进入创建的应用下从应用信息中获取appid，注意提交应用发布，审核上线后的应用才可被调用。
- 下载SDK开发包进行对接开发


## 2. SDK介绍
- SDK接口说明：

  | 名称              | 说明                                                       |
  |:----------------|:---------------------------------------------------------|
  | aiui_init       | 初始化AIUI实例                                                |
  | aiui_start      | 开启会话，一般在唤醒或者按键响应之后调用                                     |
  | aiui_send_audio | 发送音频数据（1ch 16k 16bit pcm音频数据流, 建议每次发送40ms音频量, 也就是1280字节） |
  | aiui_stop       | 停止数据发送, 断开会话                                             |
  | aiui_uninit     | 销毁AIUI交互实例, 释放相应资源                                       |

  - aiui 事件回调函数原型:
    `typedef void (*aiui_event)(const void *user_param, aiui_callback_event_t *event);`
    - user_param：用户在调用 `aiui_init` 函数传入的自定义参数
    - event：事件数据结构体指针
      - event_type: 事件类型
      - result: 事件类型对应的数据结果. 具体的参见 `demo.c` 中 `process_aiui_event` 函数中的解析流程.

- SDK调用流程：
  * 持续对话: 适用于有回消且持续对话的场景, 通常使用唤醒词触发流程
    > 初始化(`aiui_init`) `-->` 启动会话(`aiui_start`) `-->` 写音频(`aiui_send_audio`) `-->` 回调收到tts音频送到播放器播放.
    >  * `-->` 若收到云端 Vad:Bos 前端点事件, 则清空当前播放的tts音频流, 打断停止播放
    >  * `-->` 若收到云端错误码(比如触发敏感词审核), 则关闭会话(`aiui_stop`), 如需开启新会话则调用`aiui_start`.
    >  * `-->` 若不再需要交互, 释放资源(`aiui_uninit`)
  * 单次交互: 适用于无回消, 或按键触发交互的场景
    > 初始化(`aiui_init`) `-->` 按键触发启动会话(`aiui_start`) `-->` 写音频(`aiui_send_audio`) `-->` 回调收到tts音频送到播放器播放, 若收到最后一帧音频(dts==2)时断开会话(aiui_stop)
    >  * `-->` 如需继续交互或者等按键再次触发时, 再次启动会话(`aiui_start`) `-->` ...
    >  * `-->` 若不再需要交互, 释放资源(`aiui_uninit`)

- 源代码说明（SDK以源码方式提供, 需要直接将源文件加入到工程中编译）:

  | 头文件                                | 内容说明                                                                                                      |
  |:-----------------------------------|:----------------------------------------------------------------------------------------------------------|
  | aiui_api.h                         | aiui交互接口                                                                                                  |
  | aiui_log.h                         | log打印接口                                                                                                   |
  | aiui_socket_api.h                  | socket接口的二次封装, 某些平台需要在.c文件自行实现相应函数                                                                        |
  | aiui_http_api.h                    | http相关接口, 调用 aiui_socket_api.h  实现.                                                                       |
  | aiui_websocket_api.h               | websocket相关接口, 基于 aiui_http_api.h  实现, 若所处设备平台提供了websocket接口, 可将 aiui_websocket_api.c 中的实现指向平台websocket接口 |
  | aiui_message_parse.h               | 对云端下发的json数据进行解析, 解析出识别(iat), 语义(nlp), 合成(tts)结果                                                          |
  | base64.h, cjosn.h, md5.h, sha256.h | base64编解码, json解析以及Hash计算。如果与系统本身自带的函数有冲突, 先删掉, 然后在代码中引用系统自带接口即可                                          |

  与头文件对应的`.c`文件在`src/aiui`目录下。



## 3. 参数配置说明

在 `tool/deploy/bin/aiui.json` 配置文件中定义了SDK配置参数
```json
{
    "login": {
        "appid": "",
        "key": "",
        "api_secret": ""
    },
    "global": {
        "scene": "main_box",
        "aiui_ver": "3",
        "uid": "12345678abcdefgh"
    },
    "tts": {
        "voice_name": "",
        "data_encoding": "raw"
    }
}
```

<table>
	<tr>
		<td><b>模块名称</b></td>
		<td><b>模块说明</b></td>
		<td><b>参数名称</b></td>
		<td><b>是否必传</b></td>
		<td><b>参数和取值说明</b></td>
	</tr>
	<tr>
		<td rowspan="3">login</td>
		<td rowspan="3">登录参数</td>
		<td>appid</td>
		<td>是</td>
		<td>AIUI应用信息appid</td>
	</tr>
	<tr>
		<td>key</td>
		<td>是</td>
		<td>AIUI应用信息appKey</td>
	</tr>
	<tr>
		<td>api_secret</td>
		<td>是</td>
		<td>AIUI应用信息apiSecret</td>
	</tr>
	<tr>
		<td rowspan="3">global</td>
		<td rowspan="3">全局参数</td>
		<td>scene</td>
		<td>是</td>
		<td><b>AIUI应用情景模式</b><br>示例main_box</td>
	</tr>
	<tr>
		<td>aiui_ver</td>
		<td>是</td>
		<td><b>AIUI交互链路指定</b><br>3：极速交互<br></td>
	</tr>
	<tr>
		<td>uid</td>
		<td>是</td>
		<td><b>用户唯一标识</b><br>不超过32字符长度（数字、字母）<br></td>
	</tr>
	<tr>
		<td rowspan="2">tts</td>
		<td rowspan="2">合成控制参数</td>
		<td>voice_name</td>
		<td>是</td>
		<td><b>合成发音人</b><br>x5_lingxiaoyue_flow</td>
	</tr>
	<tr>
		<td>data_encoding</td>
		<td>是</td>
		<td><b>合成音频格式</b><br>raw：原始pcm音频流</td>
	</tr>
	<tr>
</table>


## 4. 功能模块说明

| 模块         | 说明                                                                                                                          |
|:-----------|:----------------------------------------------------------------------------------------------------------------------------|
| 录音实现       | 实现Ubuntu环境系统录音处理，提供 1ch 16K 16bit PCM音频录音参考（开发者可基于该实现逻辑参考实现自己设备端录音实现）, linux 录音可参考 `src/audio_player`                       |
| socket通信   | 定义在 `aiui_socket_api.c` 中的接口实现websocket通信（开发者可基于自己设备平台做相应修改，大部分平台都不需要适配可直接使用）                                               |
| 播放实现       | 实现Ubuntu环境系统播放器处理，支持播放 1ch 16K 16bit合成pcm音频流（ 开发者可基于该实现逻辑参考适配自己设备端播放器控制）, linux 播音可参考 `src/audio_recorder`                  |
| OS相关接口     | 定义在 `aiui_wrappers_os.h` 中, 包括内存分配与释放/信号量/锁和线程等接口, 需要开发者针对特定的平台实现。`aiui_os_linux.c` 即为针对linux实现的参考示例代码                      |
| 加密交互       | 若需要对流量进行加密, 则需要提供依赖mbedtls实现SSL加密通信方式（程序默认关闭不启用）, 在设备端环境支持的情况下开发者如需使用SSL, 可以在 aiui_socket_api.c 中的将 `AIUI_SUPPORT_SSL` 宏打开。 |


## 5. 交互协议及功能
1. 具体协议及功能参见SDK代码, 如下是个示例请求参数: 
   ```json
      {
           "header": {
               "app_id": "", // 应用id
               "uid": "", // 用户唯一标识，用户关联用户交互历史，
               "status": 0, // 客户端发送数据的状态   0：开始；1：中间状态；2：结束
               "stmid": "1", // 交互轮数，客户端发起的交互计数，当重建连接时，建议递增，便于定位上下文
               "scene": "main_box", // 平台上，appid下面创建的情景模式
               "msc.lat": 19.65309164062,
               "msc.lng": 109.259056086,
               "ver_type": "intelligent_hdw",
               "os_sys": "linux",
               "interact_mode":"continuous" //交互模式选择，支持continuous（双工）和 continuous_vad（单工）可选，默认continuous
           },
           "parameter": {
               "iat": {
                   "iat": {
                       "encoding": "utf8",
                       "compress": "raw",
                       "format": "json"
                   },
                   "vgap":50
               },
               "nlp": {
                   "nlp": {
                       "encoding": "utf8",
                       "compress": "raw",
                       "format": "json"
                   },
                   "new_session": "true",
                   "personal":"人设 id",
                   "prompt":"你是小明，一个小学学生，热爱画画" //系统人设新设prompt信息
               },
               "tts": {
                   "vcn": "x5_lingfeiyi_flow",
                   "res_id": "xxxx",
                   "res_gender":"",
                   "speed": 50,
                   "volume": 50,
                   "pitch": 50,
                   "tts": {
                       "encoding": "raw",
                       "sample_rate": 16000,
                       "channels": 1,
                       "bit_depth": 16,
                       "frame_size": 0
                   }
               },
               "avatar": {
                   "avatar_id": "",
                   // "res_id": "",
                   "image": "",
                   "encoding": "",
                   "width": 512,
                   "height": 512
               }
           },
           "payload": {
               "audio": {
                   "status": 0,
                   "audio": "",
                   "encoding": "raw",
                   "sample_rate": 16000,
                   "channels": 1,
                   "bit_depth": 16,
                   "frame_size": 0
               }
           }
       }
   ```
2. 如上所示, 关于设置系统人设, 只需在 parameter.nlp.prompt 下设置 prompt 提示语即可生效


## 6. 快速集成体验

- 在Ubuntu linux 快速体验

  - 下载SDK开发包

  - 安装必需软件：
      ```shell
      sudo apt install cmake libjack-jackd2-dev libasound2-dev pkg-config libpulse-dev
      ```

  - 编译、运行：
      ```shell
      cd aiui_embedded_sdk
      mkdir build
      cd build
      cmake ..
      make
      cd ../tool/deploy/bin/
      ./aiui_demo
      ```

  - 语音直接对话交互体验

    > `aiui_demo` 会在启动后读取设备默认的声卡, 使用1c16k16b参数读取麦克风音频送入云端, 并将收到的TTS音频通过声卡播放出来。

- 非linux平台, 拷贝 `src/aiui` 源码到设备, 适配 `aiui_wrapper_os.h` 中的内存分配释放/信号量/线程接口，主要需要注意：

  - 录音 适配
  - websocket 通信接口适配(若平台没有websocket实现, 且平台支持标准 BSD socket 也可使用SDK自带的实现)
  - 播放器 适配: 播放TTS音频流


## 7. 注意事项
- 设备与云端的系统时间误差不应超过`+-3`分钟, 否则设备在发起连接时会失败, 云端会校验当前时间与设备之间的差值, 超过指定的误差会被拒绝连接.
  > - 具体的连接错误信息可以在日志打印中查看.   
  >   - 常见的时间误差较大在连接时, 云端会返回 `403 Forbidden`. 
  >   - 若是appid信息授权错误, 云端会返回 `401 Unauthorized`. 
  > - 所以最佳实践是设备在联网成功后先对设备进行校时(比如使用NTP校时), 校时成功后再调用AIUI.   
  >   另外通过校时也可以初步判断网络的连通性, 如果校时都不成功, 那基本可以断定网络是不通的, 也没必要调用AIUI了.

- 极速交互只支持音频上传, 下发TTS音频播报. **不支持**文本交互和文本合成.

-  调用 `aiui_start` 到收到 `AIUI_EVENT_CONNECTED_TO_SERVER` 事件会有一定延迟, 所以在调用 `aiui_start` 后, 开始采集录音并保存到缓存队列中,
    收到 `AIUI_EVENT_CONNECTED_TO_SERVER`, 才开始从缓存队列中获取数据, 调用 `aiui_send_audio` 向云端发送数据,
    收到云端错误码(通常是命中审核,敏感词类, 服务异常)应主动断连(`aiui_stop`), 停止发送数据。
    若需要继续交互, 请在 `aiui_stop` 之后再次调用 `aiui_start`.

- 场景参数scene设置。一般地, 对于平台上创建的场景A, scene=A表示使用正式环境, scene=A_box则会使用测试环境，示例 main_box 和 main 场景区别。


- 送入云端的音频应该是做过回声消除的音频, 否则设备会陷入自播自答的情况. 若设备没有接入回消算法, 可联系讯飞提供回消算法技术支持 <aiui_support@iflytek.com>


## 8. 名词解释

| sub 名称       | 说明                                                                                           |
|:-------------|:---------------------------------------------------------------------------------------------|
| event        | 云端下发的事件, 通常是人声Vad事件. Bos: 是开始说话; Eos: 是结束说话                                                  |
| iat          | 识别结果.                                                                                        |
| cbm_tidy     | 语义规整结果：请求包含多个问题时, 大模型会进行关键信息提取做意图拆分                                                          |
| cbm_semantic | 结构化语义结果：提供技能匹配结构化结果。当语义规整中吧请求结果拆分成多个意图时，可通过 `payload.cbm_meta.text` 中 `intent` 取值当前结果对应哪个意图。 |
| nlp          | 大模型语义结果：流式下发                                                                                 |
| tts          | 合成结果：流式下发合成音频流                                                                               |
