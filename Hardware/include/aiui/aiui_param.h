#ifndef AIUI_PARAM_H
#define AIUI_PARAM_H

/**

{
    "header": {
        "code": 0,
        "message": "success",
        "sid": "xgo00010205@dx192743899eb0001822-audio-1",
        "status": 0,
        "stmid":"1"
    },
    "payload": {
        "event": {
            "compress": "",
            "encoding": "",
            "format": "",
            "seq": 0,
            "status": 0,
            "text": "eyJ0eXBlIjoiVmFkIiwiZGF0YSI6IiIsImtleSI6IkJvcyIsImRlc2MiOnt9fQ=="
        },
        "iat": {
            "compress": "raw",
            "encoding": "utf8",
            "format": "json",
            "seq": 1,
            "status": 2,
            "text": "eyJ0ZXh0Ijp7InNuIjoxLCJscyI6ZmFsc2UsImJnIjowLCJyZyI6bnVsbCwiZWQiOjAsInBncyI6IiIsInJzdCI6IiIsInNpZ24iOiIiLCJ3cyI6W3siYmciOjAsImN3IjpbeyJzYyI6MCwidyI6IuS9oCIsInBoIjoiIn1dfSx7ImJnIjowLCJjdyI6W3sic2MiOjAsInciOiLlj6siLCJwaCI6IiJ9XX0seyJiZyI6MCwiY3ciOlt7InNjIjowLCJ3Ijoi5LuA5LmIIiwicGgiOiIifV19LHsiYmciOjAsImN3IjpbeyJzYyI6MCwidyI6IuWQjeWtlyIsInBoIjoiIn1dfV19fQ=="
        },
         "nlp": {
            "compress": "",
            "encoding": "",
            "format": "",
            "seq": 0,
            "status": 0,
            "text": "5bGV56S65LqG5LiA5Liq"
        },
         "tts": {
            "compress": "",
            "encoding": "",
            "format": "",
            "seq": 0,
            "status": 0,
            "audio": "Base64 audio data"
        },
         "cbm_vms": {
            "compress": "",
            "encoding": "",
            "format": "",
            "seq": 0,
            "status": 0,
            "text": "Base64 vms connect/start/stop info"
        },
    }
}

*/

/*

{
    "header": {
        "cid": null,
        "code": 0,
        "message": "success",
        "sid": "xgo000d17a3@dx19400b1aaf40001822-audio-3",
        "status": 1,
        "stmid": "audio-3"
    },
    "payload": {
        "iat": {
            "compress": "raw",
            "encoding": "utf8",
            "format": "json",
            "seq": 1,
            "status": 2,
            "text": "{\"text\":{\"sn\":1,\"ls\":false,\"bg\":0,\"rg\":null,\"ed\":0,\"voice\":[{\"sty\":\"\\u003c说话\\u003e\",\"lev\":\"\",\"description\":\"\"}],\"pgs\":\"\",\"rst\":\"\",\"sign\":\"\",\"ws\":[{\"bg\":0,\"cw\":[{\"sc\":0,\"w\":\"你好。\",\"ph\":\"\"}]}]}}"
        }
    }
}
 */


/* sub业务状态及参数 */
typedef struct aiui_event_result_param_s
{
    // iat/tts/nlp/event ...
    char sub[16];
    // data status: 0,1,1,1,,,2. 从 payload.${sub}.status 中提取
    int dts;
    // data seq. 从 payload.${sub}.seq 中提取
    int dseq;
    // data encoding. 从 payload.${sub}.encoding 中提取. 兜底是 utf8, 音频可能是 raw/pcm
    char dte[16];
    // data format. 从 payload.${sub}.format 中提取. 兜底是 json, 音频可能是 audio/L16;rate=16000
    char dtf[32];
    // main content. extract from data. iat/tts 等sub已解析好的raw内存结果, 若不满足需求, 可从下方 data 字段中提取.
    void *key_data;
    // size of key_data
    size_t key_data_size;
} aiui_event_result_param_t;

/**
 *  AIUI_EVENT_RESULT 回调结果.
 *
 * 结构体指针在回调结束后就会被释放, 不可携带指针到其他位置.
 * 如有需要请自行拷贝.
 */
typedef struct aiui_event_result_s
{
    /* header 会话相关参数*/
    struct
    {
        int code;
        // stmid: stream id
        int stm_id;
        // session status. 0,1,1,1,,,,2
        int status;
        char sid[48];
    } session;

    /* 修饰 data 的状态/格式, 及已提取的内容 */
    aiui_event_result_param_t params;

    // data 一般存放的是 payload.${sub} 下的内容. 默认已经 base64 decode, 但如果是tts音频之类raw则不解码.
    // ///// {"data":[{"content":[{"cnt_id":"0","dte":"utf8"}],"params":{"sub":"nlp"}}]}
    // sub(iat) 可能会下发多次, 通常 ls = true 表示最后一次下发. 但极速交互下的ls/sn没有含义, 当nlp来的时候, 之前拼接的iat就是此次最终结果. 拼接所有的ws数组中的第一个cw中的w字段内容即为此次识别的最终结果.
    // iat:    {"iat":{"compress":"raw","encoding":"utf8","format":"json","seq":1,"status":2,"text":"{\"text\":{\"sn\":1,\"ls\":false,\"bg\":0,\"rg\":null,\"ed\":0,\"voice\":[{\"sty\":\"\\u003c说话\\u003e\",\"lev\":\"\",\"description\":\"\"}],\"pgs\":\"\",\"rst\":\"\",\"sign\":\"\",\"ws\":[{\"bg\":0,\"cw\":[{\"sc\":0,\"w\":\"你好。\",\"ph\":\"\"}]}]}}"}}}
    // nlp:    {"nlp":{"compress":"raw","encoding":"utf8","format":"json","seq":9,"status":1,"text":"你好"}}
    // event:  {"event":{"compress":"","encoding":"","format":"","seq":0,"status":0,"text":"{\"type\":\"Vad\",\"data\":\"\",\"key\":\"Bos\",\"desc\":{}}"}}
    // tts:    {"tts":{"audio":"base64_audio_placeholder","bit_depth":16,"channels":1,"encoding":"raw","frame_size":0,"sample_rate":16000,"seq":1,"status":0}  // 0/1/2 status
    char *data;       // <-- detail json string.
    size_t data_size; // <-- content size of data.
} aiui_event_result_t;


#endif //AIUI_PARAM_H
