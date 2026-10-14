import {describe,it,expect} from 'vitest';
import {trustedOrigin} from '../server/origins';
import {providerURL} from '../server/provider-policy';

describe('explicit trust boundaries',()=>{
  const login='http://127.0.0.1:8787/api/auth/login';

  it('accepts private-LAN origins in development, whatever this machine\'s address is',()=>{
    /*
     * The dev server is reached over the host's LAN address, which changes with
     * every network it joins. Listing addresses by hand broke the console on
     * each move, so development accepts any loopback or private address.
     */
    expect(trustedOrigin('http://192.168.137.1:5173',login,'development')).toBe(true);
    expect(trustedOrigin('http://10.34.20.209:5173',login,'development')).toBe(true);
    expect(trustedOrigin('http://172.16.5.4:5173',login,'development')).toBe(true);
    expect(trustedOrigin('http://169.254.10.1:5173',login,'development')).toBe(true);
    expect(trustedOrigin('http://localhost:5173',login,'development')).toBe(true);
    expect(trustedOrigin('http://127.0.0.1:5173',login,'development')).toBe(true);
  });

  it('never extends that to a deployed instance',()=>{
    expect(trustedOrigin('http://192.168.137.1:5173',login,'production')).toBe(false);
    expect(trustedOrigin('http://10.34.20.209:5173',login,'production')).toBe(false);
  });

  it('still rejects anything that is not a private address',()=>{
    /* Public domains cannot present a private origin, and DNS rebinding cannot
     * either: Origin carries the name the page loaded from, not the address it
     * resolved to. These are the shapes that try to look private anyway. */
    for (const origin of [
      'https://evil.example.org',
      'http://11.34.20.209:5173',        // just outside 10/8
      'http://193.168.1.1:5173',         // just outside 192.168/16
      'http://172.32.0.1:5173',          // just outside 172.16/12
      'http://127.0.0.1.evil.org:5173',  // prefix match, not an address
      'http://192.168.1.1:5173/',        // not a bare origin
      'http://user@192.168.1.1:5173',    // credentials
      'http://192.168.1.1:5173/?x=1',    // query
      'ftp://192.168.1.1',
      'http://999.168.1.1:5173',
    ]) {
      expect(trustedOrigin(origin,login,'development'),origin).toBe(false);
    }
    expect(trustedOrigin(undefined,'http://localhost:8787','development')).toBe(false);
    expect(trustedOrigin('null','http://localhost:8787','development')).toBe(false);
  });

  it('lets an administrator name an origin the address rule would not cover',()=>{
    /* A tunnel or a hostname resolving to a private address is neither
     * same-origin nor a private literal, so it still needs listing. */
    const tunnel='https://desk.example.org';
    expect(trustedOrigin(tunnel,login,'development')).toBe(false);
    expect(trustedOrigin(tunnel,login,'development',tunnel)).toBe(true);
    expect(trustedOrigin(tunnel,login,'production',tunnel)).toBe(false);
  });

  it('allows exact same origin in production',()=>{
    expect(trustedOrigin('https://desk.example.org','https://desk.example.org/api/login','production')).toBe(true);
    expect(trustedOrigin('https://evil.example.org','https://desk.example.org/api/login','production')).toBe(false);
  });
  it('restricts providers to exact administrator-approved hosts',()=>{
    expect(providerURL('https://api.deepseek.com')).toBe('https://api.deepseek.com');
    expect(()=>providerURL('https://api.deepseek.com.evil.org')).toThrow();
    expect(()=>providerURL('https://arbitrary.example.org')).toThrow();
    expect(providerURL('https://custom.example.org/v1','custom.example.org')).toBe('https://custom.example.org/v1');
    expect(()=>providerURL('https://127.0.0.1','127.0.0.1')).toThrow();
  });
});
