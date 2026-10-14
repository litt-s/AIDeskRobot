export interface User {id:string;username:string}
export interface Model {id:string;name:string;base_url:string;model:string;key_mask:string;created_at:string}
export interface Role {id:string;name:string;system_prompt:string;task:string;model_id:string|null}
export interface Conversation {id:string;role_id:string|null;title:string;updated_at:string}
export interface Message {id:string;role:'user'|'assistant';content:string;status:string;created_at:string}
