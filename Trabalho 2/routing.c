#include "routing.h"

//Funcao para imprimir mensagens de erro e encerrar o programa
void die(char* msg){
  printf("%s\n", msg);
  exit(1);
};

//Funcao para converter uma string para inteiro
int toint(char *str){
  int i, pot, ans;
  ans = 0;
  for(i = strlen(str) - 1, pot = 1; i >= 0; i--, pot *= 10)
    ans += pot * (str[i] - '0');
  return ans;
}

//Função com rotina de inicialização dos roteadores
void initialize(int id, int *port, int *sock, char adress[MAX_ADRESS], struct sockaddr_in *si_me,
                struct sockaddr_in *si_send, int neigh_list[NROUT], neighbour_t neigh_info[NROUT],
                int *neigh_qtty, dist_t routing_table[NROUT][NROUT], pack_queue_t *in, pack_queue_t *out,
                pthread_mutex_t *log_mutex, pthread_mutex_t *messages_mutex, pthread_mutex_t *news_mutex){
  int new_id, new_port, u, v, w, i, j;
  char tmp[MAX_ADRESS];

  //Inicializa o vetor de informações de vizinhos e a tabela de roteamento
  for(i = 0; i < NROUT; i++){
    neigh_info[i].news = 0;
    neigh_info[i].id = neigh_info[i].port = -1;
    neigh_info[i].cost = INF;
    for(j = 0; j < NROUT; j++){
      routing_table[i][j].dist = INF;
      routing_table[i][j].nhop = -1;
    }
  }

  //Abre o arquivo de enlaces, carrega informações sobre seus vizinhos
  FILE *links_file = fopen("enlaces.config", "r");
  if(!links_file) die("Falha ao abrir arquivo de enlaces");
  while(fscanf(links_file, "%d %d %d\n", &u, &v, &w) != EOF){
    if(v == id) {v = u; u = id;}
    if(u == id){
      neigh_list[(*neigh_qtty)++] = v;
      neigh_info[v].id = v;
      neigh_info[v].cost = neigh_info[v].orig_cost = w;
    }
  }

  //Abre o arquivo de roteadores, e Le dele a porta e o endereço do roteador
  FILE *routers_file = fopen("roteador.config", "r");
  if(!routers_file) die("Falha ao abrir arquivo de roteadores");
  while(fscanf(routers_file, "%d %d %s\n", &new_id, &new_port, tmp) != EOF){
    if(new_id == id){
      *port = new_port;
      strcpy(adress, tmp);
    }
    if(neigh_info[new_id].id != -1){
      neigh_info[new_id].port = new_port;
      strcpy(neigh_info[new_id].adress, tmp);
    }
  }
  fclose(routers_file);

  //Custo de um nó para ele mesmo é 0, via ele mesmo
  routing_table[id][id].dist = 0;
  routing_table[id][id].nhop = id;

  //Preenche o vetor de distâncias inicial do nó
  for(i = 0; i < *neigh_qtty; i++){
    u = id; v = neigh_list[i];
    routing_table[u][v].dist = neigh_info[v].cost;
    routing_table[u][v].nhop = neigh_info[v].id;
  }

  //Inicializa as filas de entrada e saida
  in->begin = out->begin = in->end = out->end = 0;
  pthread_mutex_init(&(in->mutex), NULL);
  pthread_mutex_init(&(out->mutex), NULL);

  //Inicializa mutex dos arquivos de log e mensagens e de checagem de queda de nó
  pthread_mutex_init(log_mutex, NULL);
  pthread_mutex_init(messages_mutex, NULL);
  pthread_mutex_init(news_mutex, NULL);


  //Cria o socket(dominio, tipo, protocolo)
  if((*sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
    die("Falha ao criar Socket\n");

  //Inicializa endereços de estrutura
  memset((char *) si_me, 0, sizeof(*si_me)); //Zera a estrutura
  si_me->sin_family = si_send->sin_family = AF_INET; //Familia
  si_me->sin_addr.s_addr = si_send->sin_addr.s_addr = htonl(INADDR_ANY); //Atribui o socket a todo tipo de interface
  si_me->sin_port = htons(*port); //Porta em ordem de bytes de rede

  //Liga o socket a porta (atribui o endereço ao file descriptor)
  if( bind(*sock , (struct sockaddr*) si_me, sizeof(*si_me) ) == -1)
    die("A ligacao do socket com a porta falhou\n");

}

//Imprime informações sobre o roteador
void info(int id, int port, char adress[MAX_ADRESS], int neigh_qtty, int neigh_list[NROUT],
    neighbour_t neigh_info[NROUT], dist_t routing_table[NROUT][NROUT]){
  int i;

  printf("O nó %d, está conectado à porta %d, Seu endereço é %s\n\n", id, port, adress);
  printf("Seus vizinhos são:\n");
  for(i = 0; i < neigh_qtty; i++)
    printf("O roteador %d, com enlace de custo %d, na porta %d, e endereço %s\n", neigh_list[i],
      neigh_info[neigh_list[i]].cost, neigh_info[neigh_list[i]].port, neigh_info[neigh_list[i]].adress);
  printf("\n");

  printf("Essa é sua tabela de roteamento, atualmente:\n");
  print_rout_table(routing_table, NULL, 0);
}

//Copia o pacote a para o pacote b
void copy_package(package_t *a, package_t *b){
  int i;

  b->control = a->control;
  b->dest = a->dest;
  b->orig = a->orig;
  strcpy(b->message, a->message);
  for(i = 0; i < NROUT; i++){
    b->dist_vector[i].dist = a->dist_vector[i].dist;
    b->dist_vector[i].nhop = a->dist_vector[i].nhop;
  }
}

//Enfilera um pacote para cada vizinho do nó, contendo seu vetor de distancia
void queue_dist_vec(pack_queue_t *out, int neigh_list[NROUT], dist_t routing_table[NROUT][NROUT],
                    int id, int neigh_qtty){
  int i, j;

  pthread_mutex_lock(&(out->mutex));
  for(i = 0; i < neigh_qtty; i++, out->end++){
    package_t *pck = &(out->queue[out->end]);
    pck->control = 1;
    pck->orig = id;
    pck->dest = neigh_list[i];
    //printf("Enfileirando pacote de vetor de distancia para o destino %d\n", pck->dist);
    //printf("Vetor de distancia enviado: ");
    for(j = 0; j < NROUT; j++){
      pck->dist_vector[j].dist = routing_table[id][j].dist;
      pck->dist_vector[j].nhop = routing_table[id][j].nhop;
      //printf("(%d,%d) ", pck->dist_vector[j].dist, pck->dist_vector[j].nhop);
    }
  }
  pthread_mutex_unlock(&(out->mutex));
}

//Funcao que imprime tudo que está numa fila
void print_pack_queue(pack_queue_t *queue){
  int i, j;
  package_t *pck;

  pthread_mutex_lock(&(queue->mutex));
  printf("%d Pacotes nessa fila:\n", queue->end - queue->begin);
  for(i = queue->begin; i < queue->end; i++){
    pck = &(queue->queue[i]);
    printf("\nPacote %s de controle, Destino: %d, Origem: %d\n", pck->control ? "é" : "não é", pck->dest, pck->orig);
    if(pck->control){
      printf("Vetor de distância do pacote: ");
      for(j = 0; j < NROUT; j++){
        printf("(%d,%d), ", pck->dist_vector[j].dist, pck->dist_vector[j].nhop);
      }
      printf("\n");
    }
    else printf("Mensagem do pacote: %s\n", pck->message);
  }
  printf("\n");
  pthread_mutex_unlock(&(queue->mutex));
}

//Funcao que imprime a tabela de roteamento
void print_rout_table(dist_t routing_table[NROUT][NROUT], FILE *file, int infile){
  int i, j;

  if(infile) fprintf(file, "   ");
  else printf("   ");
  for(i = 0; i < NROUT; i++){
    if (infile) fprintf(file, "  %d %s|",i, i > 0 ? " " : "");
    else printf("  %d %s|",i, i > 0 ? " " : "");
  }
  if(file) fprintf(file, "\n");
  else printf("\n");
  for(i = 0; i < NROUT; i++){
    if(file) fprintf(file, "%d |", i);
    else printf("%d |", i);
    for(j = 0; j < NROUT; j++){
      if(infile){
        if(routing_table[i][j].dist != INF) fprintf(file, "%d(%d)| ", routing_table[i][j].dist, routing_table[i][j].nhop);
        else fprintf(file, "I(X)| ");
      }
      else{
        if(routing_table[i][j].dist != INF) printf("%d(%d)| ", routing_table[i][j].dist, routing_table[i][j].nhop);
        else printf("I(X)| ");
      }
    }
    if(file) fprintf(file, "\n");
    else printf("\n");
  }
}

void print_file(FILE *file, pthread_mutex_t *mutex){
  char str[100];
  pthread_mutex_lock(mutex);
  fseek(file, 0, SEEK_SET);
  while(fgets(str, 100, file)){
    printf("%s", str);
  }
  fseek(file, 0, SEEK_END);
  pthread_mutex_unlock(mutex);
}
