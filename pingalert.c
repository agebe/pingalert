#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <curl/curl.h>
#include <argp.h>
#include <syslog.h>
#include <string.h>

#define MAX_TARGETS 1024
// set to true when too many targets have been specified
bool tooManyTargets = false;

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
  char* execNotify;
  char* execWarn;
} Arguments;

Arguments args;

char* targetName(Target* target) {
  return target->name!=NULL?target->name:target->address;
}

char* targetGroup(Target* target) {
  return target->group!=NULL?target->group:args.group;
}

const char *argp_program_version = "pingalert 0.2.0-SNAPSHOT";
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
    { "notify", 'n', "<path>", 0, "execute program when a service goes up or down"},
    { "warn", 'w', "<path>", 0, "execute program when a service is about to go down (fails test before reaching max-fail)"},
    { 0 }
};

bool startsWith(const char *string, const char *prefix) {
  return strncmp(string, prefix, strlen(prefix)) == 0?true:false;
}

void addTarget(char* arg) {
  if(args.targets >= MAX_TARGETS) {
    tooManyTargets = true;
    return;
  }
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
    case 'n': arguments->execNotify = arg; break;
    case 'w': arguments->execWarn = arg; break;
    case ARGP_KEY_ARG: return 0;
    default: return ARGP_ERR_UNKNOWN;
  }
  return 0;
}

static struct argp argp = { options, parse_opt, args_doc, doc, 0, 0, 0 };

void datetime(char* buf, size_t bufSize) {
  time_t rawtime;
    struct tm * timeinfo;
    time ( &rawtime );
    timeinfo = localtime ( &rawtime );
    snprintf(buf, bufSize, "%d-%02d-%02dT%02d:%02d:%02d",
           timeinfo->tm_year + 1900,
           timeinfo->tm_mon + 1,
           timeinfo->tm_mday,
           timeinfo->tm_hour,
           timeinfo->tm_min,
           timeinfo->tm_sec);
}

// https://www.ozzu.com/wiki/504927/writing-a-custom-printf-wrapper-function-in-c
void die(const char* format, ...) {
  va_list plist;
  va_start(plist, format);
  if(args.syslog) {
    vsyslog(LOG_ERR, format, plist);
  } else {
    char buf[1024];
    datetime(buf, sizeof(buf));
    fprintf(stderr, "%s ", buf);
    vfprintf(stderr, format, plist);
  }
  va_end(plist);
  exit(1);
}

void warn(const char* format, ...) {
  va_list plist;
  va_start(plist, format);
  if(args.syslog) {
    vsyslog(LOG_WARNING, format, plist);
  } else {
    char buf[1024];
    datetime(buf, sizeof(buf));
    printf("%s WARN ", buf);
    vprintf(format, plist);
   }
  va_end(plist);
}

void info(const char* format, ...) {
  va_list plist;
  va_start(plist, format);
  if(args.syslog) {
    vsyslog(LOG_INFO, format, plist);
  } else {
    // https://stackoverflow.com/questions/1716296/why-does-printf-not-flush-after-the-call-unless-a-newline-is-in-the-format-strin
    char buf[1024];
    datetime(buf, sizeof(buf));
    printf("%s ", buf);
    vprintf(format, plist);
   }
  va_end(plist);
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
    die("fork failed\n");
  } else if (p == 0) {
    dup2(pipe_arr[1], STDOUT_FILENO);
    execl(pingPath, "ping", "-c", "1", ip, (char*)NULL);
    die("exec ping failed\n");
  }
  int exitStatus;
  if(waitpid(p, &exitStatus, 0) == -1) {
    die("waitpid failed\n");
  }
  close(pipe_arr[0]);
  close(pipe_arr[1]);
  if(WIFEXITED(exitStatus) ) {
    const int es = WEXITSTATUS(exitStatus);
    return es==0?true:false;
  }
  die("WIFEXITED failed\n");
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
    die("init curl failed\n");
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
      info("notifyre send sms post call returned status '%ld'\n", httpStatus);
    } else {
      info("notifyre send sms post call failed\n");
    }
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
  } else {
    die("init curl failed\n");
    exit(1);
  }
}

void sendSMS(char* smsMsg, char* group) {
  char sms[160];
  snprintf(sms, sizeof(sms), "%s%s", args.prefix, smsMsg);
  if(args.notifyreApiToken == NULL) {
    warn("no notifyre api key has been set (option --notifyre-api-key), SMS notifications '%s' can't be send\n", sms);
  } else if(group != NULL) {
    info("send SMS '%s' to group '%s'\n", sms, group);
    sendSMSCurl(sms, group);
  } else {
    info("WARN, can't send SMS '%s', no group has been setup for target\n", sms);
  }
}

void execNotify(Target* target, bool up) {
  if(args.execNotify == NULL) {
    return;
  }
  pid_t p = fork();
  if(p == -1) {
    die("fork failed\n");
  } else if (p == 0) {
    execl(args.execNotify, args.execNotify, btos(up), target->type, target->url, target->address, target->name, target->group, (char*)NULL);
    die("exec notify '%s' failed\n", args.execNotify);
  }
}

void execWarn(Target* target) {
  if(args.execWarn == NULL) {
    return;
  }
  pid_t p = fork();
  char count[10];
  snprintf(count, sizeof(count), "%d", target->failed);
  if(p == -1) {
    die("fork failed\n");
  } else if (p == 0) {
    execl(args.execWarn, args.execWarn, count, target->type, target->url, target->address, target->name, target->group, (char*)NULL);
    die("exec warn '%s' failed\n", args.execWarn);
  }
}

void serviceUp(Target* target) {
  if(target->failed > 0) {
   info("service '%s' is back to normal\n", target->url);
 }
 if(target->failed >= args.maxfail) {
   char msg[1024];
   snprintf(msg, sizeof(msg), "service '%s' is back to normal", targetName(target));
   sendSMS(msg, targetGroup(target));
   execNotify(target, true);
 }
 target->failed = 0;
}

void serviceDown(Target* target) {
  if(target->failed < args.maxfail) {
    target->failed++;
    if(target->failed >= args.maxfail) {
      info("ALERT, service '%s' is down\n", target->url);
      char msg[1024];
      snprintf(msg, sizeof(msg), "service '%s' is down", targetName(target));
      sendSMS(msg, targetGroup(target));
      execNotify(target, false);
    } else {
      info("WARN, service '%s' failed, count '%d'\n", target->url, target->failed);
      execWarn(target);
    }
  }
}

bool checkTarget(Target* target) {
  if(args.verbose) {
    info("check target '%s'\n", target->url);
  }
  if(strcmp("ping", target->type) == 0) {
    return pingCheck(target->address);
  } else if(strcmp("http", target->type) == 0) {
    return httpCheck(target->address);
  } else {
    die("unknown target type\n");
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
    info("target '%s' is %s\n", target->url, btos(up));
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
      info("target '%s' is %s\n", target->url, btos(up));
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
  args.interval = 60;
  args.verbose = false;
  args.syslog = false;
  args.targets = 0;
  args.prefix = "";
  args.maxfail = 5;
  args.notifyreApiToken = NULL;
  args.group = NULL;
  args.execNotify = NULL;
  args.execWarn = NULL;
  for(int i=0;i<MAX_TARGETS;i++) {
    args.target[i] = NULL;
  }
  argp_parse(&argp, argc, argv, 0, 0, &args);
  info("%s\n", argp_program_version);
  if(tooManyTargets) {
    die("too many targets, max is '%d'\n", MAX_TARGETS);
  }
  pingPath= which("ping");
  if(args.verbose) {
    info("ping path '%s'\n", pingPath);
  }
  if(pingPath == NULL) {
    die("ping executable not found, make sure ping is installed and on the $PATH\n");
  }
  if(args.notifyreApiToken == NULL) {
    warn("no notifyre api key has been set (option --notifyre-api-key), SMS notifications can't be send\n");
  }
  if(args.targets == 0) {
    warn("no targets, add at least 1 target via -t option\n");
  }
  for(int i=0;i<args.targets;i++) {
    if((args.target[i]->group == NULL) && (args.group == NULL)) {
      warn("target '%s' has no group, SMS notification can't be send for this target\n", args.target[i]->url);
    }
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
  info("interval '%d' seconds, max fail '%d'\n", args.interval, args.maxfail);
  init();
  for(;;) {
    sleep(args.interval);
    update();
  }
}
