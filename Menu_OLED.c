#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/init.h"
#include "hardware/i2c.h"
#include "ssd1306.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/timer.h"

#define WIFI_SSID "brisa-1936495"
#define WIFI_PASS "eov1kh7c"
#define THINGSPEAK_HOST "api.thingspeak.com"
#define THINGSPEAK_PORT 80

// Link do meu canal no thingspeak: https://thingspeak.mathworks.com/channels/2842337/
#define API_KEY "9ISV7MJAFX7X07ZG" // API Key do ThingSpeak

static struct tcp_pcb *tcp_client_pcb;
char request[256]; // Buffer para o HTTP GET

// Pinos e módulos controlador i2c selecionado
#define I2C_PORT i2c1
#define PINO_SCL 14
#define PINO_SDA 15

// Pinos dos botões
#define BUTTON_A_PIN 5
#define BUTTON_B_PIN 6

// Definição dos LEDs RGB
#define BLUE_LED_PIN 12   // LED azul no GPIO 12
#define RED_LED_PIN 13    // LED vermelho no GPIO 13
#define GREEN_LED_PIN 11  // LED verde no GPIO 11

#define ADC_CHANNEL_0 0
#define ADC_CHANNEL_1 1

// Configuração do pino do buzzer
#define BUZZER_PIN 21

// Configuração da frequência do buzzer (em Hz)
#define BUZZER_FREQUENCY 100

// Variável para armazenar a posição do seletor do display
uint pos_y=15;

// Variável para armazenar caso o usuário coloque uma senha errada
int t_err = 1;

// Variáveis para a senha
char senha[5] = {'0', '0', '0', '0', '0'};
char senha_digitada[5] = {'0', '0', '0', '0', '0'};

// Variável para indicar a coluna atual
int posicao_coluna = 0;

ssd1306_t disp;

// Função para usar temporizador de hardware, substitui o sleep_ms
void wait_ms(int tempo) {
  uint64_t start_time = time_us_64(); // Tempo inicial em microssegundos
  while (time_us_64() - start_time < tempo * 1000) {}
}

// Função de callback quando recebe resposta do ThingSpeak
err_t tcp_recv_callback(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    if (p == NULL) {
        printf("Conexão encerrada pelo servidor.\n");
        tcp_close(tpcb);
        return ERR_OK;
    }

    pbuf_free(p);
    return ERR_OK;
}

// Função de callback quando a conexão TCP é estabelecida
err_t tcp_connect_callback(void *arg, struct tcp_pcb *tpcb, err_t err) {
  if (err != ERR_OK) {
    printf("Erro na conexão TCP\n");
    return err;
  }

  printf("Conectado ao ThingSpeak!\n");

  // Fazendo requisição GET ao ThingSpeak e enviando o valor de t_err para o campo 1
  snprintf(request, sizeof(request),
        "GET /update?api_key=%s&field1=%d HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        API_KEY, t_err, THINGSPEAK_HOST);

  tcp_write(tpcb, request, strlen(request), TCP_WRITE_FLAG_COPY);
  tcp_output(tpcb);
  tcp_recv(tpcb, tcp_recv_callback);

  return ERR_OK;
}

// Função que resolve o DNS e conecta ao servidor
static void dns_callback(const char *name, const ip_addr_t *ipaddr, void *callback_arg) {
    if (ipaddr) {
        printf("Endereço IP do ThingSpeak: %s\n", ipaddr_ntoa(ipaddr));
        tcp_connect(tcp_client_pcb, ipaddr, THINGSPEAK_PORT, tcp_connect_callback);
    } else {
        printf("Falha na resolução de DNS\n");
    }
}

// Função de configuração do Wi-Fi
void wifi_setup() {
  if (cyw43_arch_init()) {
    printf("Falha ao inicializar o módulo Wi-Fi.\n");
    return;
  }

  cyw43_arch_enable_sta_mode();
  
  if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASS, CYW43_AUTH_WPA2_AES_PSK, 10000)) {
    printf("Não foi possível encontrar a rede Wi-Fi.\n");
    cyw43_arch_poll();
    cyw43_arch_deinit();
    wait_ms(1000);
    wifi_setup();
  }
  printf("Wi-Fi conectado com sucesso.\n");
}

// Função para enviar dados ao ThingSpeak
void enviarThingSpeak() {
  tcp_client_pcb = tcp_new();
  if (!tcp_client_pcb) {
    printf("Falha ao criar o TCP PCB.\n");
    return;
  }

  printf("Resolvendo %s...\n", THINGSPEAK_HOST);
  ip_addr_t server_ip;
  err_t err = dns_gethostbyname(THINGSPEAK_HOST, &server_ip, dns_callback, NULL);
  if (err == ERR_OK) {
    printf("DNS resolvido imediatamente: %s\n", ipaddr_ntoa(&server_ip));
    tcp_connect(tcp_client_pcb, &server_ip, THINGSPEAK_PORT, tcp_connect_callback);
  }
  else if (err != ERR_INPROGRESS) {
    printf("Falha na resolução DNS: %d\n", err);
    tcp_close(tcp_client_pcb);
  }
}

// Função para facilitar o controle dos LEDS
void led_rgb_put(bool r, bool y, bool g) {
  gpio_put(RED_LED_PIN, r);
  gpio_put(GREEN_LED_PIN, y);
  gpio_put(BLUE_LED_PIN, g);
}

// Definição de uma função para inicializar o PWM no pino do buzzer
void pwm_init_buzzer(uint pin) {
  // Configurar o pino como saída de PWM
  gpio_set_function(pin, GPIO_FUNC_PWM);

  // Obter o slice do PWM associado ao pino
  uint slice_num = pwm_gpio_to_slice_num(pin);

  // Configurar o PWM com frequência desejada
  pwm_config config = pwm_get_default_config();
  pwm_config_set_clkdiv(&config, clock_get_hz(clk_sys) / (BUZZER_FREQUENCY * 4096)); // Divisor de clock
  pwm_init(slice_num, &config, true);

  // Iniciar o PWM no nível baixo
  pwm_set_gpio_level(pin, 0);
}

// Definição de uma função para emitir um beep com duração especificada (Retirada de: https://wokwi.com/projects/417299852817197057)
void beep(uint pin, uint duration_ms) {
  // Obter o slice do PWM associado ao pino
  uint slice_num = pwm_gpio_to_slice_num(pin);
  // Configurar o duty cycle para 50% (ativo)

  pwm_set_gpio_level(pin, 2048);

  // Temporização
  wait_ms(duration_ms);

  // Desativar o sinal PWM (duty cycle 0)
  pwm_set_gpio_level(pin, 0);

  // Pausa entre os beeps
  wait_ms(100); // Pausa de 100ms
}

// Função para atualizar o display com a senha digitada
void atualizar_display() {
  // Limpa display e desenha interface principal
  ssd1306_clear(&disp);
  ssd1306_draw_string(&disp, 20, 2, 1, "Digite a Senha");
  ssd1306_draw_empty_square(&disp,15, 18, 94, 20);
  ssd1306_draw_string(&disp, 15, 50, 1, "Botao A Confirma");
    
  // Loop para exibir a senha digitada
  for (int i = 0; i < 5; i++) {
    char str[2] = {senha_digitada[i]}; // Armazena o digito da coluna
    // Aumenta a  posição X a cada iteração para os números ficarem um do lado do outro
    int x_pos = 20 + (i * 20); 

    // Desenha linha abaixo do número da coluna atualmente selecionada
    if (i == posicao_coluna) {
      ssd1306_draw_line(&disp, x_pos - 1, 35, x_pos + 5, 35);
    }
    // Exibe número referente a coluna atual na posição x_pos
    ssd1306_draw_string(&disp, x_pos, 25, 1, str);
    }
    // Manda todas atualizações pro display
    ssd1306_show(&disp);
}
// Função para controlar a navegação com o joystick
void joystick_control() {
  adc_select_input(0);
  uint adc_y_raw = adc_read();
  adc_select_input(1);
  uint adc_x_raw = adc_read();
  // Movimento para cima
  if (adc_y_raw > 4000) {
    senha_digitada[posicao_coluna]++;  // Incrementa número da senha digitada
    if (senha_digitada[posicao_coluna] > '9') {  // Volta para 0 caso ultrapasse 9
      senha_digitada[posicao_coluna] = '0';
    }
    wait_ms(200); // Debounce do botão
  }
  // Movimento para baixo
  else if (adc_y_raw < 100) {
    senha_digitada[posicao_coluna]--;  // Decrementa o número da senha digitada
    if (senha_digitada[posicao_coluna] < '0') { // Vai para 9 caso volte mais que 0
      senha_digitada[posicao_coluna] = '9';
    }
    wait_ms(200);  // Debounce do botão
  }
  // Movimento para direita
  else if (adc_x_raw > 4000) {
    // Variável referente a coluna é incrementada
    posicao_coluna++;
    if (posicao_coluna > 4) { // Volta para 0 caso a posição seja maior que 4
      posicao_coluna = 0;
    }
    wait_ms(200);  // Debounce do botão
  }
  // Movimento para esquerda
  else if (adc_x_raw < 100) {
    // Varíavel referente a coluna é decrementada
    posicao_coluna--;
    if (posicao_coluna < 0) { // Vai para posição 4 caso seja menor que 0
      posicao_coluna = 4;
    }
    wait_ms(200);  // Debounce do botão
  }
}

// Função para checar a senha após o botão ser pressionado
void passcheck() {
  // Se botão A for pressionado
  if (gpio_get(BUTTON_A_PIN) == 0) {
    // Limpa display
    ssd1306_clear(&disp);
    // Compara os valores da senha e da senha digitada
    if (memcmp(senha, senha_digitada, sizeof(senha)) == 0) {
      // Se iguais mostra Bem-Vindo ;) liga LED Verde e espera 5 segundos
      ssd1306_draw_string(&disp, 27, 25, 1,"Bem - Vindo ;)");
      ssd1306_show(&disp);
      led_rgb_put(0,1,0);
      wait_ms(5000);
    }
    else {
      // Se diferentes mostra Senha errada D: liga LED vermelho
      ssd1306_draw_string(&disp, 20, 25, 1, "Senha errada D:");
      ssd1306_show(&disp);
      led_rgb_put(1,0,0);
      // Envia o valor de t_err (1), para o ThingSpeak
      enviarThingSpeak();
      // Liga buzzer e espera 5 segundos
      beep(BUZZER_PIN, 5000);
    }
  }
}

// Função para inicialização de todos os recursos do sistema
void inicializa(){
  stdio_init_all();
  adc_init();
  adc_gpio_init(26);
  adc_gpio_init(27);
  i2c_init(I2C_PORT, 400*1000);// I2C Inicialização. Usando 400Khz.
  gpio_set_function(PINO_SCL, GPIO_FUNC_I2C);
  gpio_set_function(PINO_SDA, GPIO_FUNC_I2C);
  gpio_pull_up(PINO_SCL);
  gpio_pull_up(PINO_SDA);
  disp.external_vcc=false;
  ssd1306_init(&disp, 128, 64, 0x3C, I2C_PORT);

  // Inicialização dos LEDs
  gpio_init(RED_LED_PIN);
  gpio_init(GREEN_LED_PIN);
  gpio_init(BLUE_LED_PIN);
  gpio_set_dir(RED_LED_PIN, GPIO_OUT);
  gpio_set_dir(GREEN_LED_PIN, GPIO_OUT);
  gpio_set_dir(BLUE_LED_PIN, GPIO_OUT);

  // Inicialização dos Botões
  gpio_init(BUTTON_A_PIN);
  gpio_init(BUTTON_B_PIN);
  gpio_set_dir(BUTTON_A_PIN, GPIO_IN);
  gpio_set_dir(BUTTON_B_PIN, GPIO_IN);
  gpio_pull_up(BUTTON_A_PIN);
  gpio_pull_up(BUTTON_B_PIN);

  // Configuração do GPIO para o buzzer como saída
  gpio_init(BUZZER_PIN);
  gpio_set_dir(BUZZER_PIN, GPIO_OUT);
  // Inicializar o PWM no pino do buzzer
  pwm_init_buzzer(BUZZER_PIN);

  // Inicialmente, desligar o LED RGB
  gpio_put(RED_LED_PIN, 0);
  gpio_put(GREEN_LED_PIN, 0);
  gpio_put(BLUE_LED_PIN, 0);
}
// Função principal
int main() {
  inicializa();  // Inicializa o sistema
  wifi_setup();  // Inicializa wi-fi
  
  // Loop Principal do programa
  while (true) {
    // Inicialmente Desliga o LED RGB
    led_rgb_put(0,0,0);
    // Controla joystick
    joystick_control();
    // Mostra informações do display
    atualizar_display();
    // Checa se a senha está correta se o botão for apertado
    passcheck();
    // Delay
    wait_ms(100);
  }
  return 0;
}