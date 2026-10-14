import {existsSync,writeFileSync} from 'node:fs';
import {randomBytes} from 'node:crypto';
const path=new URL('../.dev.vars',import.meta.url);
if(!existsSync(path)){writeFileSync(path,`APP_ENV=development\nENCRYPTION_KEY=${randomBytes(32).toString('base64')}\n`);console.log('Created ignored local development secret.');}else console.log('Local configuration already exists.');
