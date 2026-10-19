const {defineConfig}=require('@playwright/test');
const distribution=process.env.HELM_TEST_DISTRIBUTION==='1';
const wclap=process.env.HELM_TEST_WCLAP==='1';
const server=wclap?[
  {command:'python3 scripts/serve.py --port 8137 --directory ../build/wclap',url:'http://127.0.0.1:8137/Helm.wclap.tar.gz',reuseExistingServer:false},
  {command:'python3 server.py 8138',cwd:'../build/wclap-test-host',url:'http://127.0.0.1:8138/',reuseExistingServer:false},
]:{command:'python3 scripts/serve.py --port 8137'+(distribution?' --directory ../build/web-dist':''),url:'http://127.0.0.1:8137/'+(distribution?'':'helm.html'),reuseExistingServer:false};
module.exports=defineConfig({
  testDir:'tests',testMatch:wclap?'wclap.spec.js':distribution?'distribution.spec.js':['browser.spec.js','wam.spec.js'],workers:1,timeout:90000,
  use:{channel:'chrome',headless:true,viewport:{width:1200,height:950},
    launchOptions:{args:['--disable-audio-output',...(wclap?['--autoplay-policy=no-user-gesture-required']:[])]},baseURL:'http://127.0.0.1:8137'},
  webServer:server,
});
