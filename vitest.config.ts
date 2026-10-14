import {defineConfig} from 'vitest/config';
import vite from './vite.config';
export default defineConfig({...vite,test:{root:'.',include:['tests/**/*.test.ts']}});
