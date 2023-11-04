
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <curl/curl.h>
#include <argp.h>
#include <syslog.h>
#include <string.h>

#define MAX_TARGETS 1024

const char* UP = "up";
const char* DOWN = "down";

// ping executable path
char* pingPath;

typedef struct Target {
  char* type;
  char* url;
  char* address;
  char* name;
  char* group;
  int failed;
} Target;

typedef struct Arguments {
  unsigned int interval;
  bool verbose;
  bool syslog;
  char* group;
  int targets;
  Target* target[MAX_TARGETS];
  char* prefix;
  unsigned int maxfail;
  char* notifyreApiToken;
} Arguments;

Arguments args;

char* targetName(Target* target) {
  return target->name!=NULL?target->name:target->address;
}

char* targetGroup(Target* target) {
  return target->group!=NULL?target->group:args.group;
}

const char *argp_program_version = "pingalert 0.1.0";
const char *argp_program_bug_address = "andre.gebers@gmail.com";
static char doc[] = "";
static char args_doc[] = "";
static struct argp_option options[] = {
    { "interval", 'i', "<interval>", 0, "check interval in seconds. defaults to 60 seconds"},
    { "verbose", 'v', 0, 0, "enable verbose output"},
    { "syslog", 's', 0, 0, "use syslog logging instead of stdout"},
    { "group", 'g', "<group ID>", 0, "the default Notifyre group ID (not the group name, determine via https://api.notifyre.com/addressbook/groups) to use when SMS notifications are send"},
    { "target", 't', "<target>", 0, "the target to check, the format is <ping|http|https>://<address>[,[service-name][,[group ID to notify]]]. specify this option multiple times to check multiple targets"},
    { "prefix", 'p', "<prefix>", 0, "sms prefix that is added to alert and back-to-normal SMS notifications"},
    { "max-fail", 'm', "<max-fail>", 0, "how many times the service check has to fail before a sms notification is send. defaults to 5"},
    { "notifyre-api-key", 'k', "<notifyre-api-key>", 0, "the http header x-api-token to use when sending sms, https://docs.notifyre.com/api/sms-send"},
    { 0 }
};

bool startsWith(const char *string, const char *prefix) {
  return strncmp(string, prefix, strlen(prefix)) == 0?true:false;
}

void addTarget(char* arg) {
  char *str;
  char *url;
  char *name;
  char *group;
  char *adr;
  str = strdup(arg);
  url = strsep(&str, ",");
  name = strsep(&str, ",");
  group = strsep(&str, ",");
  Target* target = malloc(sizeof(Target));
  if(startsWith(url, "ping://")) {
    target->type = "ping";
    adr = url + strlen("ping://");
  } else if(startsWith(url, "http://")) {
    target->type = "http";
    adr = url;
  } else if(startsWith(url, "https://")) {
    target->type = "http";
    adr = url;
  } else {
    printf("invalid target url in '%s', use either ping://<ip address> or http://<address> or https://<address>\n", arg);
    exit(1);
  }
  target->url = url;
  target->address = adr;
  target->name = name;
  target->group = group;
//  printf("type '%s'\n", target->type);
//  printf("url '%s'\n", target->address);
//  printf("name '%s'\n", target->name);
//  printf("group '%s'\n", target->group);
//  printf("\n");
  args.target[args.targets++] = target;
}

static error_t parse_opt(int key, char *arg, struct argp_state *state) {
  Arguments *arguments = state->input;
  switch (key) {
    case 'i': arguments->interval = atoi(arg); break;
    case 'v': arguments->verbose = true; break;
    case 's': arguments->syslog = true; break;
    case 'g': arguments->group = arg; break;
    case 't': addTarget(arg); break;
    case 'p': arguments->prefix = arg; break;
    case 'm': arguments->maxfail = atoi(arg); break;
    case 'k': arguments->notifyreApiToken = arg; break;
    case ARGP_KEY_ARG: return 0;
    default: return ARGP_ERR_UNKNOWN;
  }
  return 0;
}

static struct argp argp = { options, parse_opt, args_doc, doc, 0, 0, 0 };

void info(const char* msg) {
  if(args.syslog) {
    syslog(LOG_INFO, "%s\n", msg);
  } else {
    time_t rawtime;
    struct tm * timeinfo;
    time ( &rawtime );
    timeinfo = localtime ( &rawtime );
    printf("%d-%02d-%02dT%02d:%02d:%02d %s\n",
           timeinfo->tm_year + 1900,
           timeinfo->tm_mon + 1,
           timeinfo->tm_mday,
           timeinfo->tm_hour,
           timeinfo->tm_min,
           timeinfo->tm_sec,
           msg);
  }
}

const char* btos(bool b) {
  return b?UP:DOWN;
}

bool pingCheck(const char* ip) {
  // https://stackoverflow.com/a/23071412
  // https://stackoverflow.com/a/27307246
  int pipe_arr[2];
  // create pipe - pipe_arr[0] is "reading end", pipe_arr[1] is "writing end"
  pipe(pipe_arr);
  pid_t p = fork();
  if(p == -1) {
    perror("fork failed");
    exit(1);
  } else if (p == 0) {
    dup2(pipe_arr[1], STDOUT_FILENO);
    execl(pingPath, "ping", "-c", "1", ip, (char*)NULL);
    perror("exec ping failed");
    exit(1);
  }
  int exitStatus;
  if(waitpid(p, &exitStatus, 0) == -1) {
    perror("waitpid failed");
    exit(1);
  }
  close(pipe_arr[0]);
  close(pipe_arr[1]);
  if(WIFEXITED(exitStatus) ) {
    const int es = WEXITSTATUS(exitStatus);
    return es==0?true:false;
  }
  perror("WIFEXITED failed");
  exit(1);
}

static size_t cb(void *data, size_t size, size_t nmemb, void *clientp) {
  size_t realsize = size * nmemb;
  return realsize;
}

bool httpCheck(const char *url) {
  CURL *curl;
  CURLcode res;
  curl = curl_easy_init();
  if (curl) {
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cb);
    //curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);
    // https://curl.se/libcurl/c/CURLOPT_FAILONERROR.html
    // any error code like e.g. 401 means the service is alive and that is all we want to know here
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0);
    res = curl_easy_perform(curl);
    if(res == CURLE_OK) {
      long httpStatus;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
      curl_easy_cleanup(curl);
      if((httpStatus>=100) && (httpStatus<500)) {
        return true;
      } else {
        return false;
      }
    } else {
      curl_easy_cleanup(curl);
      return false;
    }
  } else {
    perror("init curl failed");
    exit(1);
  }
}

void sendSMSCurl(char* msg, char* group) {
  CURL *curl;
  CURLcode res;
  curl = curl_easy_init();
  if (curl) {
    char json[1024];
    snprintf(json, sizeof(json), "{\"Body\":\"%s\",\"Recipients\":[{\"type\":\"group\",\"value\":\"%s\"}],\"From\":\"\",\"AddUnsubscribeLink\":false}", msg, group);
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.notifyre.com/sms/send");
    if(args.verbose) {
      curl_easy_setopt(curl, CURLOPT_HEADER, 1L);
    } else {
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cb);
    }
    struct curl_slist *list = NULL;
    char tokenHeader[1024];
    snprintf(tokenHeader, sizeof(tokenHeader), "x-api-token: %s", args.notifyreApiToken);
    list = curl_slist_append(list, tokenHeader);
    list = curl_slist_append(list, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    res = curl_easy_perform(curl);
    if(res == CURLE_OK) {
      long httpStatus;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
      char log[2048];
      snprintf(log, sizeof(log), "notifyre send sms post call returned status '%ld'", httpStatus);
      info(log);
    } else {
      char log[2048];
      snprintf(log, sizeof(log), "notifyre send sms post call failed");
      info(log);
    }
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
  } else {
    perror("init curl failed");
    exit(1);
  }
}

void sendSMS(char* smsMsg, char* group) {
  char sms[160];
  snprintf(sms, sizeof(sms), "%s%s", args.prefix, smsMsg);
  if(group != NULL) {
    char msg[2048];
    snprintf(msg, sizeof(msg), "send SMS '%s' to group '%s'", sms, group);
    info(msg);
    sendSMSCurl(sms, group);
  } else {
    char msg[2048];
    snprintf(msg, sizeof(msg), "WARN, can't send SMS '%s', no group has been setup for target", sms);
    info(msg);
  }
}

void serviceUp(Target* target) {
  char msg[1024];
  if(target->failed > 0) {
   snprintf(msg, sizeof(msg), "service '%s' is back to normal", target->url);
   info(msg);
 }
 if(target->failed >= args.maxfail) {
   snprintf(msg, sizeof(msg), "service '%s' is back to normal", targetName(target));
   sendSMS(msg, targetGroup(target));
 }
 target->failed = 0;
}

void serviceDown(Target* target) {
  char msg[1024];
  if(target->failed < args.maxfail) {
    target->failed++;
    if(target->failed >= args.maxfail) {
      snprintf(msg, sizeof(msg), "ALERT, service '%s' is down", target->url);
      info(msg);
      snprintf(msg, sizeof(msg), "service '%s' is down", targetName(target));
      sendSMS(msg, targetGroup(target));
    } else {
      snprintf(msg, sizeof(msg), "WARN, service '%s' failed, count '%d'", target->url, target->failed);
      info(msg);
    }
  }
}

bool checkTarget(Target* target) {
  if(args.verbose) {
    char msg[1024];
    snprintf(msg, sizeof(msg), "check target '%s'", target->url);
    info(msg);
  }
  if(strcmp("ping", target->type) == 0) {
    return pingCheck(target->address);
  } else if(strcmp("http", target->type) == 0) {
    return httpCheck(target->address);
  } else {
    info("unknown target type");
    exit(1);
  }
}

void init() {
  for(int i=0;i<args.targets;i++) {
    Target* target = args.target[i];
    bool up = checkTarget(target);
    if(up) {
      target->failed = 0;
    } else {
      target->failed = args.maxfail;
    }
    char msg[1024];
    snprintf(msg, sizeof(msg), "target '%s' is %s", target->url, btos(up));
    info(msg);
  }
}

void update() {
  for(int i=0;i<args.targets;i++) {
    Target* target = args.target[i];
    bool up = checkTarget(target);
    if(up) {
      serviceUp(target);
    } else {
      serviceDown(target);
    }
    if(args.verbose) {
      char msg[1024];
      snprintf(msg, sizeof(msg), "target '%s' is %s", target->url, btos(up));
      info(msg);
    }
  }
}

char* which(char* name) {
  char* path = getenv("PATH");
  if(path == NULL) {
    return NULL;
  }
  int pathLen = strlen(path);
  char pc[pathLen+1];
  strcpy(pc, path);
  //printf("%s\n", pc);
  char delims[] = ":";
  char *result = NULL;
  result = strtok(pc, delims);
  char buf[64*1024];
  while(result != NULL) {
    char* dp = result;
    //printf("%s\n", dp);
    snprintf(buf, sizeof(buf), "%s/%s", dp, name);
    //printf("%s\n", buf);
    struct stat st = {0};
    if(stat(buf, &st) == 0) {
      char *p = malloc( sizeof(char) * ( strlen(buf) + 1 ) );
      strcpy(p, buf);
      return p;
    }
    result = strtok(NULL, delims);
  }
  return NULL;
}

int main(int argc, char **argv) {
  pingPath= which("ping");
  if(pingPath == NULL) {
    perror("ping executable not found\n");
    exit(1);
  }
  char msg[1024];
  args.interval = 60;
  args.verbose = false;
  args.syslog = false;
  args.targets = 0;
  args.prefix = "";
  args.maxfail = 5;
  args.notifyreApiToken = "";
  for(int i=0;i<MAX_TARGETS;i++) {
    args.target[i] = NULL;
  }
  argp_parse(&argp, argc, argv, 0, 0, &args);
  info(argp_program_version);
  if(args.verbose) {
    // TODO use info instead of printf
    printf("ping path '%s'\n", pingPath);
  }
  // TODO check that notifyre api-key is set
  if(args.targets == 0) {
    snprintf(msg, sizeof(msg), "no targets, add at least 1 target via -t option");
    info(msg);
    exit(1);
  }
  if(args.interval <= 0) {
    args.interval = 60;
  }
  if(args.interval > 86400) {
    args.interval = 60;
  }
  if(args.maxfail <= 0) {
    args.maxfail = 1;
  }
  if(args.maxfail > 1000) {
    args.maxfail = 1000;
  }
  snprintf(msg, sizeof(msg), "interval '%d' seconds, max fail '%d'", args.interval, args.maxfail);
  info(msg);
  init();
  for(;;) {
    sleep(args.interval);
    update();
  }
}
