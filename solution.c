#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <errno.h>

#define white "\x1b[0m"
#define red "\x1b[31m"
#define yellow "\x1b[33m"
#define blue "\x1b[34m"

#define MAX_PIZZAS          128
#define MAX_CHEFS           128
#define MAX_CUSTOMERS       128
#define MAX_SP_INGREDIENTS  128

#define NOT_AVAILABLE   0
#define PREPARING       1
#define AVAILABLE       2
#define LEFT            3

#define COMPLETE            4
#define PARTIALLY_PROCESSED 5
#define REJECTED            6
#define ACCEPTED            7

typedef struct {
    int chef_idx;
    int arrival_time;
    int exit_time;
    int status;

    int customer;
    int pizza_type;
    int pizza_idx; // 0 indexed
    
    pthread_mutex_t chef_lock;
    pthread_t chef_tid;
} chef;

typedef struct {
    int pizza_idx; // 0 indexed
    int customer_idx;
    int pizza_type;
    int status;

    sem_t order_processed;
} p;

typedef struct {
    int pizza_idx;
    int prep_time;
    int num_sp_ingr;
    int* sp_ingr;
} pizza_type;

typedef struct {
    int ing_idx;
    int quantity;
} sp_ingr;

typedef struct {
    int customer_idx;
    int entry_time;
    int num_pizzas;
    p* pizzas;
    int state;
    pthread_t customer_tid;
} customer;


int num_chefs, num_pizza_types, num_sp_ingr, num_customers, num_ovens, time_to_pickup;

pthread_mutex_t c_lock;
pthread_mutex_t cArr_lock;
pthread_mutex_t iArr_lock;
pthread_cond_t chefAvailable = PTHREAD_COND_INITIALIZER;

sem_t ovensAvailable;
sem_t drive_thru;
struct timespec start_time;


sp_ingr special_ingr[MAX_SP_INGREDIENTS];
pizza_type pizza_types[MAX_PIZZAS];
chef chefs[MAX_CHEFS];
customer customers[MAX_CUSTOMERS];


void* chefProcedure(void* st)
{
    chef* d_chef = (chef*) st;

    sleep(d_chef->arrival_time);
    printf("%sChef %d arrives at time %d.%s\n", blue, d_chef->chef_idx, d_chef->arrival_time, white);

    pthread_mutex_lock(&cArr_lock);

    d_chef->status = AVAILABLE;
    pthread_cond_signal(&chefAvailable); 

    pthread_mutex_unlock(&cArr_lock);

    sleep(d_chef->exit_time - d_chef->arrival_time);

    pthread_mutex_lock(&cArr_lock);
    d_chef->status = LEFT;
    pthread_mutex_unlock(&cArr_lock);

    printf("%sChef %d exits at time %d.\n%s", blue, d_chef->chef_idx, d_chef->exit_time, white);
}

void* chefRoutine(void* st)
{
    chef* d_chef = (chef*) st;
    int pizza_idx = d_chef->pizza_type - 1;

    pthread_mutex_lock(&iArr_lock);

    // for every special ingredient in the pizza
    for (int i=0; i<pizza_types[pizza_idx].num_sp_ingr; i++) 
    {
        int ing = pizza_types[pizza_idx].sp_ingr[i] - 1;

        if ( special_ingr[ing].quantity <= 0 ) {

            // ingredient not available, can't make pizza
            pthread_mutex_unlock(&iArr_lock);
            printf("%sChef %d could not complete pizza %d for order %d due to ingredient shortage.%s\n", blue, d_chef->chef_idx, d_chef->pizza_type, d_chef->customer, white);

            // customers[d_chef->customer - 1].state = PARTIALLY_PROCESSED;

            // mark the order as rejected
            customers[d_chef->customer-1].pizzas[d_chef->pizza_idx].status = REJECTED;
            sem_post(&customers[d_chef->customer-1].pizzas[d_chef->pizza_idx].order_processed);

            pthread_cond_signal(&chefAvailable);
            return (void*) 0;
        }
    }

    sem_post(&customers[d_chef->customer-1].pizzas[d_chef->pizza_idx].order_processed);

    // prepare pizza
    sleep(3);
    for (int i=0; i<pizza_types[ pizza_idx ].num_sp_ingr; i++) 
    {
        int ing = pizza_types[pizza_idx].sp_ingr[i] - 1;

            special_ingr[ing].quantity--;
    }  

    pthread_mutex_unlock(&iArr_lock);

    // make pizza
    printf("%sChef %d is waiting for oven allocation for pizza %d for order %d.\n%s", blue, d_chef->chef_idx, d_chef->pizza_type, d_chef->customer, white);
    sem_wait(&ovensAvailable);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    printf("%sChef %d has put pizza %d for order %d in the oven at time %ld.\n%s", blue, d_chef->chef_idx, d_chef->pizza_type, d_chef->customer, ts.tv_sec-start_time.tv_sec, white);
    
    sleep(pizza_types[d_chef->pizza_type-1].prep_time);

    sem_post(&ovensAvailable);

    clock_gettime(CLOCK_REALTIME, &ts);
    printf("%sChef %d has picked up the pizza %d for order %d from the oven at time %ld.\n%s", blue, d_chef->chef_idx, d_chef->pizza_type, d_chef->customer, ts.tv_sec-start_time.tv_sec, white);

    sem_post(&customers[d_chef->customer-1].pizzas[d_chef->pizza_idx].order_processed);

    pthread_mutex_lock(&cArr_lock);
    d_chef->status = AVAILABLE;
    pthread_mutex_unlock(&cArr_lock);
    pthread_cond_signal(&chefAvailable);
}

void* allotChef(void* st)
{
    p* d_pizza = (p*) st;

CHEF_CHECK:

    // INGREDIENT CHECK
    pthread_mutex_lock(&iArr_lock);

    int pizza_idx = d_pizza->pizza_type-1;

    // for every special ingredient in the pizza
    for (int i=0; i<pizza_types[pizza_idx].num_sp_ingr; i++) 
    {
        int ing = pizza_types[pizza_idx].sp_ingr[i] - 1;

        if ( special_ingr[ing].quantity <= 0 ) {

            // ingredient not available, can't make pizza
            d_pizza->status = REJECTED;

            pthread_mutex_unlock(&iArr_lock);

            sem_post(&d_pizza->order_processed);

            pthread_cond_signal(&chefAvailable);
            return (void*) 0;
        }
    }
    pthread_mutex_unlock(&iArr_lock);


    int chef_acquired = 0;
    pthread_mutex_lock(&cArr_lock);

    for (int i=0; i<num_chefs; i++) {
        if (chefs[i].status == AVAILABLE) {
            
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            int curr_time = ts.tv_sec - start_time.tv_sec;

            if (chefs[i].exit_time - curr_time >= pizza_types[d_pizza->pizza_type - 1].prep_time+3) 
            {    
                d_pizza->status = ACCEPTED;

                printf("%sPizza %d in order %d has been assigned to Chef %d.%s\n", red, d_pizza->pizza_type, d_pizza->customer_idx, chefs[i].chef_idx, white);
                
                chefs[i].status = PREPARING;

                chefs[i].pizza_idx = d_pizza->pizza_idx;
                chefs[i].customer = d_pizza->customer_idx;
                chefs[i].pizza_type = d_pizza->pizza_type;

                pthread_t chef_make_pizza;
                pthread_create(&chef_make_pizza, NULL, chefRoutine, &chefs[i]);

                chef_acquired = 1;
            } 
        }
    }
    pthread_mutex_unlock(&cArr_lock);

    if (!chef_acquired)
    {
        pthread_mutex_lock(&cArr_lock);
        pthread_cond_wait(&chefAvailable, &cArr_lock);
        pthread_mutex_unlock(&cArr_lock);

        goto CHEF_CHECK;  
    }
}

void* customerProcedure(void* st)
{
    customer* d_customer = (customer*) st;
    sleep(d_customer->entry_time);
    printf("%sCustomer %d arrives at time %d.\n%s", yellow, d_customer->customer_idx, d_customer->entry_time, white);
    printf("%sCustomer %d is waiting for the drive-thru allocation.\n%s", yellow, d_customer->customer_idx, white);
    printf("%sCustomer %d enters the drive-thru zone and gives out their order %d.\n%s", yellow, d_customer->customer_idx, d_customer->customer_idx, white);

    printf("%sOrder %d placed by customer %d has pizzas {%d", red, d_customer->customer_idx, d_customer->customer_idx, d_customer->pizzas[0].pizza_type);
    for (int i=1; i<d_customer->num_pizzas; i++) printf(", %d", d_customer->pizzas[i].pizza_type);
    printf("}.\n%s", white);

    printf("%sOrder %d placed by customer %d awaits processing.\n%s", red, d_customer->customer_idx, d_customer->customer_idx, white);

    pthread_t pArr[d_customer->num_pizzas];
    for (int i=0; i<d_customer->num_pizzas; i++)
        pthread_create(&pArr[i], NULL, allotChef, (void*) &d_customer->pizzas[i]);

    printf("%sOrder %d placed by customer %d is being processed.\n%s", red, d_customer->customer_idx, d_customer->customer_idx, white);

    for (int i=0; i<d_customer->num_pizzas; i++)
        pthread_join(pArr[i], NULL);

    for (int i=0; i<d_customer->num_pizzas; i++) {
        sem_wait(&d_customer->pizzas[i].order_processed);
    }

    int pCount = 0;
    for (int i=0; i<d_customer->num_pizzas; i++)
    {
        if (d_customer->pizzas[i].status != REJECTED) pCount++;
    }
    if (pCount == 0) {// rejected
        d_customer->state = REJECTED;
        printf("%sOrder %d placed by customer %d completely rejected.\n%s", red, d_customer->customer_idx, d_customer->customer_idx, white);
        printf("%sCustomer %d exits the drive-thru zone.\n%s", yellow, d_customer->customer_idx, white);
        return (void*) 0;

    } else if (pCount < d_customer->num_pizzas) {
        d_customer->state = PARTIALLY_PROCESSED;
        printf("%sOrder %d placed by customer %d partially processed and remaining couldn’t be.\n%s", red, d_customer->customer_idx, d_customer->customer_idx, white);

    } else if (pCount == d_customer->num_pizzas) {
        d_customer->state = COMPLETE;
        printf("%sOrder %d placed by customer %d has been processed.\n%s", red, d_customer->customer_idx, d_customer->customer_idx, white);
    }

    sleep(time_to_pickup);

    printf("%sCustomer %d is waiting at the pickup spot.\n%s", yellow, d_customer->customer_idx, white);

    for (int i=0; i<d_customer->num_pizzas; i++) {
        if (d_customer->pizzas[i].status != REJECTED) {
            sem_wait(&d_customer->pizzas[i].order_processed);
            printf("%sCustomer %d picks up their pizza %d.\n%s", yellow, d_customer->customer_idx, d_customer->pizzas[i].pizza_type, white);
        }
    }
    printf("%sCustomer %d exits the drive-thru zone.\n%s", yellow, d_customer->customer_idx, white);
}

int main()
{
    scanf(" %d %d %d %d %d %d", &num_chefs, &num_pizza_types, &num_sp_ingr, &num_customers, &num_ovens, &time_to_pickup);

    sem_init(&drive_thru, 0, 1);
    sem_init(&ovensAvailable, 0, num_ovens);
    pthread_mutex_init(&c_lock, NULL);

    pthread_mutex_init(&cArr_lock, NULL);
    pthread_mutex_init(&iArr_lock, NULL);
    clock_gettime(CLOCK_REALTIME, &start_time);

    // pizza types
    for (int i=0; i<num_pizza_types; i++)
    {
        scanf(" %d %d %d", &pizza_types[i].pizza_idx, &pizza_types[i].prep_time, &pizza_types[i].num_sp_ingr);
        pizza_types[i].sp_ingr = calloc(pizza_types[i].num_sp_ingr, sizeof(int));

        for (int j=0; j<pizza_types[i].num_sp_ingr; j++)
            scanf(" %d", &pizza_types[i].sp_ingr[j]);
    }

    // quantities of special ingredients
    for (int i=0; i<num_sp_ingr; i++)
    {
        special_ingr[i].ing_idx = i+1;
        scanf(" %d", &special_ingr[i].quantity);
    }
    
    // chef timings
    for (int i=0; i<num_chefs; i++)
    {
        chefs[i].chef_idx = i+1;
        chefs[i].status = NOT_AVAILABLE;
        chefs[i].pizza_idx = -1;
        pthread_mutex_init(&chefs[i].chef_lock, NULL);

        scanf(" %d %d", &chefs[i].arrival_time, &chefs[i].exit_time);
    }

    // customer details (assuming every customer will order at least one pizza)
    for (int i=0; i<num_customers; i++)
    {
        customers[i].customer_idx = i+1;
        customers[i].state = -1;
        scanf(" %d %d", &customers[i].entry_time, &customers[i].num_pizzas);

        customers[i].pizzas = (p*) calloc(customers[i].num_pizzas, sizeof(p));

        for (int j=0; j<customers[i].num_pizzas; j++) {
            scanf(" %d", &customers[i].pizzas[j].pizza_type);
            sem_init(&customers[i].pizzas[i].order_processed, 0, 0);

            customers[i].pizzas[j].pizza_idx = j;
            customers[i].pizzas[j].customer_idx = customers[i].customer_idx;
            customers[i].pizzas[j].status = NOT_AVAILABLE;
        }
    }

    printf("Simulation Started.\n");

    for (int i=0; i<num_chefs; i++) {
        pthread_create(&chefs[i].chef_tid, NULL, chefProcedure, (void*) &chefs[i]);
    }

    for (int i=0; i<num_customers; i++) {
        pthread_create(&customers[i].customer_tid, NULL, customerProcedure, (void*) &customers[i]);
    }

    for (int i=0; i<num_chefs; i++) {
        pthread_join(chefs[i].chef_tid, NULL);
    }

    printf("Simulation Over.\n");
}

/*
3 3 3 4 5 3
1 20 3 1 2 3
2 30 2 2 3
3 30 0
10 5 2
0 50 20 60 30 120
0 2 1 2
1 1 1
2 2 1 2
4 1 3
*/