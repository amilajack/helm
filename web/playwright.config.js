const {defineConfig}=require('@playwright/test');
const distribution=process.env.HELM_TEST_DISTRIBUTION==='1';
module.exports=defineConfig({
  testDir:'tests',testMatch:distribution?'distribution.spec.js':'browser.spec.js',workers:1,timeout:90000,
  use:{channel:'chrome',headless:true,viewport:{width:1200,height:950},
    launchOptions:{args:['--disable-audio-output']},baseURL:'http://127.0.0.1:8137'},
  webServer:{command:'python3 scripts/serve.py --port 8137'+(distribution?' --directory ../build/web-dist':''),url:'http://127.0.0.1:8137/'+(distribution?'':'helm.html'),reuseExistingServer:false},
});
