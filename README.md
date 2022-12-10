
# Concurrency in C

### Assumptions

- If a chef is waiting for oven allocation, and has to exit before he gets an oven, the resolution is unclear so it is assumed that such a situation will not occur and need not be handled.
- Since the capacity of the drive-thru is equal to the number of total customers, the orders of all customers are processed in parallel.
- Maximum values for the given entities are assumed.

### Algorithm

* Customer arrives at the given time.
* He waits for drive-thru allocation, as is instantly allocated to drive-thru.
* He places his order
* Order is 'processed'- we check is this order is serviceable.
	* If we have enough ingredients
	* If we have available chefs with enough time
* If the order is serviceable
	* Customer waits at pick-up spot.
	* We look/wait for chef assignment.
	* When we get a chef, chef checks for ingredients again, where there is a possibility of rejection.
	* If we get all ingredients, we again look/wait for an oven.
	* Finally, after oven allocation, we starting baking the pizza.
	* Once pizza cooked, it is instantly put at the pick-up spot and picked up by customer.
- If the order is not serviceable
	- We mark the pizza as rejected.
- If all of a customer's pizza are rejected, they instantly exit the drive-thru.
- Else, they wait at the pickup spot for their pizzas that were accepted.


### Implementational Details

- Each unique entity is stored in an array of its own:
```c
sp_ingr special_ingr[MAX_SP_INGREDIENTS];
pizza_type pizza_types[MAX_PIZZAS];
chef chefs[MAX_CHEFS];
customer customers[MAX_CUSTOMERS];
```

- We have a mutex lock for the ingredient array and the chef array.

- Each customer starts off as a thread with the `customerProcedure()`.
- Each chef thread just marks the status of a chef as `AVAILABLE` once he is supposed to enter, and as `LEFT` once he leaves the restaurant.

- In `customerProcedure()`:
	- We first take care of its arrival business- done by putting the thread to sleep till arrival time
	- We begin processing his order by creating a thread for this purpose, using the `allotChef()` function.
	- We wait till the thread returns, telling us if the pizzas in the order were serviceable or not.
		```c
		for (int i=0; i<d_customer->num_pizzas; i++) {
			sem_wait(&d_customer->pizzas[i].order_processed);
		} 
	
		int pCount = 0;
		for (int i=0; i<d_customer->num_pizzas; i++) {
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
		```
	- Customer is instantly rejected if the entire order was unserviceable, and exits the drive-thru.
	- Else, the customer waits on the semaphore for each pizza in his order, so he can collect it.
	- Once he has all pizzas, he leaves the drive-thru.
	
- In `allotChef()`:
	- Performs a check for availability of special ingredients for a particular pizza.
	- If ingredients are available, performs a check for availability of a chef with enough time to service this pizza.
	- If a chef can't currently service this order, we wait conditionally on a variable called `chefAvailable`.
		```c
		if (!chef_acquired) {
			pthread_mutex_lock(&cArr_lock);
			pthread_cond_wait(&chefAvailable, &cArr_lock);
			pthread_mutex_unlock(&cArr_lock);
			goto CHEF_CHECK;
		}
		```
- 
	- `chefAvailable` is signaled whenever a chef is freed up from a particular task.
	- Once a chef is acquired, we create a thread with `chefRoutine()`.
	
- In `chefRoutine()`:
	- We perform a check for special ingredients once again.
	- Finally post to the customer if this particular pizza from his order is serviceable.
		- Customer was waiting on the semaphore for each pizza in his order.
		- He can now know whether to wait for his pizzas or just leave.
	- Perform the actions to make the pizza, such as decrementing the number of ingredients and sleeping for the required arrangement time.
	- Wait (on the `ovensAvailable` semaphore) for an oven to be available.
	- Bake the pizza, i.e. sleep for `prep_time` number of seconds.
	- Post to the `ovensAvailable` semaphore, to let other chefs know that an oven is now free.
	- Conditional signal to `chefAvailable` because a chef is now free, to let other orders be prepared.
	- The pizza is now at the pickup spot, which is informed to the customer by posting to a semaphore that the customer was waiting on.


### Follow up questions

1. The pick-up spot now has a stipulated amount of pizzas it can hold. If the pizzas are full, chefs route the pizzas to a secondary storage. How would you handle such a situation?

	We lock both the secondary storage and the pick-up spot, and move a pizza whenever the pick-up spot has a vacancy. Moreover, if a pizza is newly made, and the pick-up spot has just one vacancy, we first move a pizza from the secondary storage to the pick-up spot, before moving a freshly baked one to the pick-up spot. We instead put the freshly baked pizza in the secondary storage. This way we ensure that every pizza reaches the pick-up spot. 
		
***

2. Each incomplete order affects the ratings of the restaurant. Given the past histories of orders, how would you re-design your simulation to have lesser incomplete orders? Note that the rating of the restaurant is not affected if the order is rejected instantaneously on arrival.

	From the past histories of orders, we can obtain the most common and least common pizzas that are ordered. This way, we know the probability of a pizza of a particular type being serviced. Thus, we can instantly all pizzas that have a low probability of being serviced. Therefore, by only servicing pizzas that are more likely to get serviced, we will have lesser incomplete orders.
***

3. Ingredients can be replenished on calling the nearest supermarket. How would your drive-thru rejection / acceptance change based on this?

	We can accept the order and then replenish the ingredients, but as a we wait for them, we must continue assigning chefs to other orders and processing other orders, taking into account the time it would take for fresh ingredients to arrive.
	However, if the time taken for ingredients is too much, and all chefs would leave by then, we instant reject that pizza.
